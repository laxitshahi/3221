#include <pthread.h>
#include <time.h>
#include "errors.h"
#include <semaphore.h>

void *display_thread(void *arg);
void create_display_thread(int group_id);

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
    int original_group_id;  // To track changes in group ID
    int message_changed;    // Flag to indicate if the message has changed
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

typedef struct removed_alarm_tag
{
    struct removed_alarm_tag *link;
    int alarm_id;
    int group_id;
    time_t removal_time;
    char message[129];
} removed_alarm_t;

#define MAX_DISPLAY_THREADS 10
display_thread_info_t display_threads[MAX_DISPLAY_THREADS];

pthread_cond_t alarm_cond = PTHREAD_COND_INITIALIZER;
alarm_t *alarm_list = NULL;
change_alarm_t *change_alarm_list = NULL;
removed_alarm_t *removed_alarm_list = NULL;
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
     * LOCKING PROTOCOL
     */

    // Pointers to iterate through linked list
    last = &alarm_list;
    next = *last;

    // Iterate through alarm_list (linked list)
    while (next != NULL)
    {
        // If we find an alarm that fits in the correct slot, place alarm in linked list and break iteration
        if (next->alarm_id >= alarm->alarm_id)
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

    // On successful insertion, print message to user
    printf("Alarm(%d) Inserted by Main Thread %p Into Alarm List at %ld: Group(%d) %s\n",
           alarm->alarm_id, pthread_self(), (long)time(NULL), alarm->group_id, alarm->message);

    /*
     * Wake the alarm thread if it is not busy (that is, if
     * current_alarm is 0, signifying that it's waiting for
     * work), or if the new alarm comes before the one on
     * which the alarm thread is waiting.
     */

    pthread_cond_signal(&alarm_cond);

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
    // Logic is the same as standard alarm_list insert
    change_alarm_t **last, *next;

    sem_wait(&change_list_sem); // Wait on the semaphore before accessing change_alarm_list

    // LOCKING PROTOCOL:
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

void assign_alarm_to_display_thread(alarm_t *alarm)
{
    int assigned = 0;
    // First we check if there is already a thread that is associated with the group_id of the given alarm
    for (int i = 0; i < MAX_DISPLAY_THREADS; i++)
    {
        // Ensure that the thread is active and the group_id of the alarm is equal to the groiup_id of the active display_thread
        if (display_threads[i].active == 1 && display_threads[i].group_id == alarm->group_id)
        {
            // Each display thread (which is associated with a particular group_id) should have MAX TWO alarms associated to it
            // If there is < 2 alarms associated with this particular display threads...
            if (display_threads[i].alarm_count < 2)
            {

            // 1. Increment the number of alarms this particular display thread handles
                display_threads[i].alarm_count++;
            // 2. Set assign flag to 1 so that a new thread is not create below
                assigned = 1;
            // 3. Print coresponding message to user
                printf("Main Thread %p Assigned to Display Alarm(%d) at %ld: Group(%d) %s\n",
                       pthread_self(), alarm->alarm_id, (long)time(NULL), alarm->group_id, alarm->message);
                break;
            }
        }
    }

    // BUT If
    // 1. If there does not exist any display thread associated with the alarm's group_id or..
    // 2. If the all display threads associated with the given alarm's group_id already have more than 1 alarm associated with it
    // Then create a new display_thread
    // @note, it is possible that multiple there are the same group_ids across multiple threads
    if (!assigned)
    {
        // New display thread created with the associated group_id
        create_display_thread(alarm->group_id);
        // Update the number of assigned threads to 1
        alarm->assigned_to_thread = 1;
        // Print corresponding message to user
        printf("Main Thread Created New Display Alarm Thread %p For Alarm(%d) at %ld: Group(%d) %s\n",
               pthread_self(), alarm->alarm_id, (long)time(NULL), alarm->group_id, alarm->message);
    }
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
            removed_alarm_t *removed_alarm = (removed_alarm_t *)malloc(sizeof(removed_alarm_t));
            if (removed_alarm != NULL)
            {
                removed_alarm->alarm_id = current_alarm->alarm_id;
                removed_alarm->group_id = current_alarm->group_id;
                removed_alarm->removal_time = now;
                strncpy(removed_alarm->message, current_alarm->message, sizeof(removed_alarm->message) - 1);
                removed_alarm->link = removed_alarm_list;
                removed_alarm_list = removed_alarm;
            }
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
                    if (alarm->group_id != change->group_id)
                    {
                        alarm->original_group_id = alarm->group_id;
                        alarm->group_id = change->group_id;
                        assign_alarm_to_display_thread(alarm); // Reassign the alarm
                    }
                    if (strcmp(alarm->message, change->message) != 0)
                    {
                        strncpy(alarm->message, change->message, sizeof(alarm->message) - 1);
                        alarm->message_changed = 1;
                    }
                    alarm->time = change->time; // Update the time of the alarm
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

void create_display_thread(int group_id)
{
    // Loop through each display thread
    for (int i = 0; i < MAX_DISPLAY_THREADS; i++)
    {
      // ONLY if this display thread is inactive (active == 0)
        if (display_threads[i].active == 0)
        {
          // Setup display thread with given group_id
            display_threads[i].group_id = group_id;
            display_threads[i].active = 1;
            display_threads[i].alarm_count = 1;

          
            // Allocate space for the display thread 
            int *arg = malloc(sizeof(int));
            *arg = group_id;

            // create new thread and pass in group_id into display_thread (thread function) WITH the group_id
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

        

        // Logic for when an alarm is removed
        removed_alarm_t **last = &removed_alarm_list, *current;

        // Iterate through all removed alarms
        while ((current = *last) != NULL)
        {
            // If the given group_id (from args) is equal to any of the removed alarms...
            if (current->group_id == group_id)
            {
              // 1. Print message to users to notify them that the thread is no longer with the particular alarm
                printf("Display Thread %p Has Stopped Printing Message of Alarm(%d) at %ld: Group(%d) %s\n",
                       pthread_self(), current->alarm_id, (long)current->removal_time, current->group_id, current->message);
                
                *last = current->link;
              // Free memory used by alarm in removed_alarm_list
                free(current);
            }
          
            // If no alarm is found that matches the group_id, simply iterate to the next value to check again
            else
            {
                last = &current->link;
            }
        }



        
        int found = 0;
        time_t now = time(NULL);

        // Iterate over the alarm list and print messages for the matching group
        for (alarm_t *alarm = alarm_list; alarm != NULL; alarm = alarm->link)
        {
            if (alarm->group_id == group_id)
            {
                /*
                 * if alarm's group_id has been changed:
                 * print message to user to notify and reset flag to unchanged (0)
                */
                if (alarm->original_group_id != 0)
                {
                    printf("Display Thread %p Has Stopped Printing Message of Alarm(%d) at %ld: Changed Group(%d) %s\n",
                           pthread_self(), alarm->alarm_id, (long)now, alarm->original_group_id, alarm->message);
                    alarm->original_group_id = 0; // Reset after printing
                }
                /*
                 * if alarm's group_id has been changed:
                 * print message to user to notify and reset flag to unchanged (0)
                */
                  else if (alarm->message_changed)
                  {
                    printf("Display Thread %p Starts to Print Changed Message Alarm(%d) at %ld: Group(%d) %s\n",
                           pthread_self(), alarm->alarm_id, (long)now, alarm->group_id, alarm->message);
                    alarm->message_changed = 0; // Reset after printing
                }
                
                // If there have be no changes to the alarm's group_id or message, print normal message that prints every 5 seconds
                else
                {
                    // Normal printing of the message
                    int time_left = (int)difftime(alarm->time, now);
                    printf("Alarm (%d) Printed by Alarm Display Thread %p at %ld: Group(%d) %d %s\n",
                           alarm->alarm_id, pthread_self(), (long)now, alarm->group_id, time_left, alarm->message);
                }
                // Update found flag to ensure that thread does not exit
                found = 1;
            }
        }

        sem_post(&alarm_list_sem); // Post to the semaphore after reading alarm_list

        // But If no alarms were found (0) for the group, exit the thread
        if (!found)
        {
            printf("No More Alarms in Group(%d): Display Thread %p exiting at %ld\n",
                   group_id, pthread_self(), (long)now);
            break;
        }

        // Sleep for 5 seconds as per the requirements (show message only every 5 seconds)
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


    // Start always running pthread that handles alarms
    status = pthread_create(
        &thread, NULL, alarm_thread, NULL);

    // If creation of alarm fails, throw err and abort
    if (status != 0)
        err_abort(status, "Create alarm thread");

    // Constantly running (Always looking for user input)
    while (1)
    {
        printf("Alarm> ");
        if (fgets(line, sizeof(line), stdin) == NULL)
            exit(0);

        line[strcspn(line, "\n")] = 0; // Remove newline character

        // If the user inputs 1 or less character, restart loop
        if (strlen(line) <= 1)
            continue;

      
        // If "Start_Alarm" command is called
        if (strncmp(line, "Start_Alarm", 11) == 0)
        {
            // Allocate memory for alarm
            alarm_t *alarm = (alarm_t *)malloc(sizeof(alarm_t));
          // if allocation fails, throw err and abort
            if (alarm == NULL)
                errno_abort("Allocate alarm");

            // ensure all parts of the Start_Alarm command are correct
            // If any parts of the Start_Alarm command is inconsistent with expected result, print error and free space allocated
            if (sscanf(line, "Start_Alarm(%d): Group(%d) %d %128[^\n]",
                       &alarm->alarm_id, &alarm->group_id, &alarm->seconds, alarm->message) < 4)
            {
                fprintf(stderr, "Bad Start_Alarm command\n");
                free(alarm);
            }

            // If all inputs are correct, setup alarm, assign to display thread, and insert into alarm_list
            else
            {
                alarm->time = time(NULL) + alarm->seconds;
                assign_alarm_to_display_thread(alarm);
                alarm_insert(alarm);
            }
        }

        // If command for "Change_Alarm" called
        // If portion of input format is incorrect, free memory and print error
        else if (strncmp(line, "Change_Alarm", 12) == 0)
        {
            // Setup memory for change_alarm alarm
            change_alarm_t *change_alarm = (change_alarm_t *)malloc(sizeof(change_alarm_t));

            // if allocation fails, throw err and abort
            if (change_alarm == NULL)
                errno_abort("Allocate change alarm");

            int seconds;
            // ensure all parts of the Start_Alarm command are correct
            // If any parts of the Change_Alarm command is inconsistent with expected result, print error and free space allocated
            if (sscanf(line, "Change_Alarm(%d): Group(%d) %d %128[^\n]",
                       &change_alarm->alarm_id, &change_alarm->group_id, &seconds, change_alarm->message) < 4)
            {
                fprintf(stderr, "Bad Change_Alarm command\n");
                free(change_alarm);
            }

            // If all inputs are correct, setup alarm and insert into change_alarm_list
            else
            {
                change_alarm->time = time(NULL) + seconds;
                change_alarm_insert(change_alarm);
            }
        }

        // Reject all other inputs
        else
        {
            fprintf(stderr, "Invalid command\n");
        }
    }

    return 0;
}
