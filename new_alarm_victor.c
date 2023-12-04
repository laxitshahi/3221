/*
 * alarm_cond.c
 *
 * This is an enhancement to the alarm_mutex.c program, which
 * used only a mutex to synchronize access to the shared alarm
 * list. This version adds a condition variable. The alarm
 * thread waits on this condition variable, with a timeout that
 * corresponds to the earliest timer request. If the main thread
 * enters an earlier timeout, it signals the condition variable
 * so that the alarm thread will wake up and process the earlier
 * timeout first, requeueing the later request.
 */
#include <pthread.h>
#include <time.h>
#include "errors.h"
#include <semaphore.h>

void *display_thread(void *arg);
void create_display_thread(int group_id);

/*
 * The "alarm" structure now contains the time_t (time since the
 * Epoch, in seconds) for each alarm, so that they can be
 * sorted. Storing the requested number of seconds would not be
 * enough, since the "alarm thread" cannot tell how long it has
 * been on the list.
 */
typedef struct alarm_tag
{
    struct alarm_tag *link;
    int seconds;
    int alarm_id;
    int group_id;
    int type;               // 0 for Start_Alarm, 1 for Change_Alarm
    time_t time;            /* seconds from EPOCH */
    char message[129];      // Increased size to 128 characters
    int assigned_to_thread; // 0: not assigned, 1: assigned
} alarm_t;

typedef struct change_alarm_tag
{
    struct change_alarm_tag *link;
    int alarm_id;
    int group_id;
    time_t time;
    char message[129];
} change_alarm_t;

typedef struct display_thread_info_tag
{
    pthread_t thread_id;
    int group_id;
    int active;      // 0: inactive, 1: active
    int alarm_count; // Number of alarms assigned to this thread
} display_thread_info_t;

#define MAX_DISPLAY_THREADS 10
display_thread_info_t display_threads[MAX_DISPLAY_THREADS];

// pthread_mutex_t alarm_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t alarm_cond = PTHREAD_COND_INITIALIZER;
alarm_t *alarm_list = NULL;
change_alarm_t *change_alarm_list = NULL;
time_t current_alarm = 0;

sem_t alarm_list_sem;  // Semaphore for alarm list
sem_t change_list_sem; // Semaphore for change alarm list

/*
 * Insert alarm entry on list, in order.
 */
void alarm_insert(alarm_t *alarm)
{
    int status;
    alarm_t **last, *next;

    sem_wait(&alarm_list_sem); // Wait on the semaphore before accessing alarm_list

    /*
     * LOCKING PROTOCOL:
     *
     * This routine requires that the caller have locked the
     * alarm_mutex!
     */
    last = &alarm_list;
    next = *last;
    while (next != NULL)
    {
        if (next->time >= alarm->time)
        {
            alarm->link = next;
            *last = alarm;
            break;
        }
        last = &next->link;
        next = next->link;
    }
    /*
     * If we reached the end of the list, insert the new alarm
     * there.  ("next" is NULL, and "last" points to the link
     * field of the last item, or to the list header.)
     */
    if (next == NULL)
    {
        *last = alarm;
        alarm->link = NULL;
    }

    sem_post(&alarm_list_sem); // Post to the semaphore after modifying alarm_list

    printf("Alarm(%d) Inserted by Main Thread %p Into Alarm List at %ld: Group(%d) %s\n",
           alarm->alarm_id, pthread_self(), (long)time(NULL), alarm->group_id, alarm->message);

    pthread_cond_signal(&alarm_cond);

#ifdef DEBUG
    printf("[list: ");
    for (next = alarm_list; next != NULL; next = next->link)
        printf("%d(%d)[\"%s\"] ", next->time,
               next->time - time(NULL), next->message);
    printf("]\n");
#endif
    /*
     * Wake the alarm thread if it is not busy (that is, if
     * current_alarm is 0, signifying that it's waiting for
     * work), or if the new alarm comes before the one on
     * which the alarm thread is waiting.
     */
    if (current_alarm == 0 || alarm->time < current_alarm)
    {
        current_alarm = alarm->time;
        status = pthread_cond_signal(&alarm_cond);
        if (status != 0)
            err_abort(status, "Signal cond");
    }
}

void change_alarm_insert(change_alarm_t *change_alarm)
{
    change_alarm_t **last, *next;

    sem_wait(&change_list_sem); // Wait on the semaphore before accessing change_alarm_list

    // LOCKING PROTOCOL:
    // Assumes that the caller has locked the alarm_mutex

    last = &change_alarm_list;
    next = *last;
    while (next != NULL)
    {
        if (next->alarm_id >= change_alarm->alarm_id)
        {
            change_alarm->link = next;
            *last = change_alarm;
            break;
        }
        last = &next->link;
        next = next->link;
    }

    // If we reached the end of the list, insert the new change alarm there
    if (next == NULL)
    {
        *last = change_alarm;
        change_alarm->link = NULL;
    }

    sem_post(&change_list_sem); // Post to the semaphore after modifying change_alarm_list

    printf("Change Alarm Request (%d) Inserted by Main Thread %p into Change Alarm List at %ld: Group(%d) %s\n",
           change_alarm->alarm_id, pthread_self(), (long)time(NULL), change_alarm->group_id, change_alarm->message);
}

/*
 * The alarm thread's start routine.
 */
void *alarm_thread(void *arg)
{
    while (1)
    {
        alarm_t *current_alarm = NULL;
        time_t now = time(NULL);
        int status;

        // Wait on the semaphores before accessing the lists
        sem_wait(&alarm_list_sem);
        sem_wait(&change_list_sem);

        // Process and remove expired alarms
        while (alarm_list != NULL && alarm_list->time <= now)
        {
            current_alarm = alarm_list;
            alarm_list = alarm_list->link;
            printf("Alarm Monitor Thread %p Has Removed Alarm(%d) at %ld: Group(%d) %s\n",
                   pthread_self(), current_alarm->alarm_id, (long)now, current_alarm->group_id, current_alarm->message);
            free(current_alarm);
            current_alarm = NULL;
        }

        // Process Change_Alarm requests
        change_alarm_t *change = change_alarm_list;
        // While there exists a "change_alarm_list" continue looking for alarms with:
        // same alarm_id
        while (change != NULL)
        {
            // Flag to check if a corresponding alarm between alarm_list and change_alarm_list exists
            int found_pair = 0;
            // Find the alarm with the same Alarm_ID and apply changes
            for (alarm_t *alarm = alarm_list; alarm != NULL; alarm = alarm->link)
            {
                if (alarm->alarm_id == change->alarm_id)
                {
                    alarm->group_id = change->group_id;
                    alarm->time = change->time;
                    strncpy(alarm->message, change->message, sizeof(alarm->message) - 1);
                    printf("Alarm Monitor Thread %p Has Changed Alarm(%d) at %ld: Group(%d) %s\n",
                           pthread_self(), alarm->alarm_id, (long)time(NULL), alarm->group_id, alarm->message);

                    found_pair = 1;
                    // We want to save the alarm in change_alarm_list, so break
                    break;
                }
            }
            // If there was no corresponding alarm found, then we print error
            if (!found_pair)
            {
                printf("Invalid Change Alarm Request(%d) at %ld: Group(%d) %s\n",
                       change->alarm_id, (long)time(NULL), change->group_id, change->message);
            }

            // Remove the alarm used to update the alarm in the alarm_list from change_alarm_list
            change_alarm_t *temp = change;
            change = change->link;
            free(temp);
        }
        change_alarm_list = NULL;

        // Post to the semaphores after modifying the lists
        sem_post(&change_list_sem);
        sem_post(&alarm_list_sem);

        // Sleep or wait for a condition to efficiently use CPU
        sleep(1); // Adjust sleep time as needed
    }

    return NULL; // Return statement to avoid compiler warnings
}

void assign_alarm_to_display_thread(alarm_t *alarm)
{
    int assigned = 0;
    for (int i = 0; i < MAX_DISPLAY_THREADS; i++)
    {
        // Ensure that alarm is associated with the CORRECT group ID
        if (display_threads[i].active == 1 && display_threads[i].group_id == alarm->group_id)
        {
            // Each display thread (which is associated with a particular group_id) should have MAX TWO alarms associated to it
            if (display_threads[i].alarm_count < 2)
            {
                display_threads[i].alarm_count++;
                assigned = 1;
                printf("Main Thread %p Assigned to Display Alarm(%d) at %ld: Group(%d) %s\n",
                       pthread_self(), alarm->alarm_id, (long)time(NULL), alarm->group_id, alarm->message);
                break;
            }
        }
    }

    // If
    // 1. If there does not exist any display thread associated with the alarm's group_id
    // 2. If the all display threads associated with the given alarm's group_id already have more than 1 alarm associated with it
    // Then create a new display_thread
    // @note, it is possible that multiple there are the same group_ids across multiple threads
    if (!assigned)
    {
        create_display_thread(alarm->group_id);
        alarm->assigned_to_thread = 1;
        printf("Main Thread Created New Display Alarm Thread %p For Alarm(%d) at %ld: Group(%d) %s\n",
               pthread_self(), alarm->alarm_id, (long)time(NULL), alarm->group_id, alarm->message);
    }
}

void create_display_thread(int group_id)
{
    for (int i = 0; i < MAX_DISPLAY_THREADS; i++)
    {
        if (display_threads[i].active == 0)
        {
            display_threads[i].group_id = group_id;
            display_threads[i].active = 1;
            display_threads[i].alarm_count = 1;

            int *arg = malloc(sizeof(int));
            *arg = group_id;
            pthread_create(&display_threads[i].thread_id, NULL, display_thread, arg);
            // Add error handling for pthread_create
            break;
        }
    }
}

void *display_thread(void *arg)
{
    int group_id = *(int *)arg;
    free(arg); // Assuming group_id was dynamically allocated

    while (1)
    {
        sem_wait(&alarm_list_sem); // Wait on the semaphore before accessing alarm_list

        int found = 0;
        time_t now = time(NULL);

        // Iterate over the alarm list and print messages for the matching group
        for (alarm_t *alarm = alarm_list; alarm != NULL; alarm = alarm->link)
        {
            if (alarm->group_id == group_id && alarm->time > now)
            {
                printf("Alarm (%d) Printed by Alarm Display Thread %p at %ld: Group(%d) %s\n",
                       alarm->alarm_id, pthread_self(), (long)now, alarm->group_id, alarm->message);
                found = 1;
            }
        }

        sem_post(&alarm_list_sem); // Post to the semaphore after reading alarm_list

        // If no alarms were found for the group, exit the thread
        if (!found)
        {
            printf("No More Alarms in Group(%d): Display Thread %p exiting at %ld\n",
                   group_id, pthread_self(), (long)now);
            break;
        }

        // Sleep for 5 seconds as per the requirements
        sleep(5);
    }

    return NULL;
}

int main(int argc, char *argv[])
{
    int status;
    char line[256]; // Increased line length for longer messages
    alarm_t *alarm;
    pthread_t thread;

    sem_init(&alarm_list_sem, 0, 1);  // Initialize semaphore for alarm list
    sem_init(&change_list_sem, 0, 1); // Initialize semaphore for change alarm list

    status = pthread_create(
        &thread, NULL, alarm_thread, NULL);
    if (status != 0)
        err_abort(status, "Create alarm thread");
    while (1)
    {
        printf("Alarm> ");
        if (fgets(line, sizeof(line), stdin) == NULL)
            exit(0);

        line[strcspn(line, "\n")] = 0; // Remove newline character

        if (strlen(line) <= 1)
            continue;

        if (strncmp(line, "Start_Alarm", 11) == 0)
        {
            // Allocate memory for alarm
            alarm_t *alarm = (alarm_t *)malloc(sizeof(alarm_t));
            if (alarm == NULL)
                errno_abort("Allocate alarm");

            // Check if all inputs are correct for Start_Alarm
            // If portion of input format is incorrect, free memory and print error
            if (sscanf(line, "Start_Alarm(%d): Group(%d) %d %128[^\n]",
                       &alarm->alarm_id, &alarm->group_id, &alarm->seconds, alarm->message) < 4)
            {
                fprintf(stderr, "Bad Start_Alarm command\n");
                free(alarm);
            }
            // If all inputs are correct..
            else
            {
                alarm->time = time(NULL) + alarm->seconds;
                assign_alarm_to_display_thread(alarm);
                alarm_insert(alarm);
            }
        }
        else if (strncmp(line, "Change_Alarm", 12) == 0)
        {
            change_alarm_t *change_alarm = (change_alarm_t *)malloc(sizeof(change_alarm_t));
            if (change_alarm == NULL)
                errno_abort("Allocate change alarm");

            int seconds;
            if (sscanf(line, "Change_Alarm(%d): Group(%d) %d %128[^\n]",
                       &change_alarm->alarm_id, &change_alarm->group_id, &seconds, change_alarm->message) < 4)
            {
                fprintf(stderr, "Bad Change_Alarm command\n");
                free(change_alarm);
            }
            else
            {
                change_alarm->time = time(NULL) + seconds;
                change_alarm_insert(change_alarm);
            }
        }
        else
        {
            fprintf(stderr, "Invalid command\n");
        }
    }
    // Cleanup and exit code
    return 0;
}