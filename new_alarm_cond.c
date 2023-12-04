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
    time_t time; /* seconds from EPOCH */
    char message[64];
    int alarm_id;
    int group_number;
} alarm_t;

pthread_mutex_t alarm_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t alarm_cond = PTHREAD_COND_INITIALIZER;
alarm_t *alarm_list = NULL;
time_t current_alarm = 0;

/*
 * Insert alarm entry on list, in order.
 */
void alarm_insert(alarm_t *alarm)
{
    int status;
    alarm_t **last, *next;

    /*
     * LOCKING PROTOCOL:
     * This routine requires that the caller have locked the
     * alarm_mutex!
     */
    last = &alarm_list; // Points to the address that points to the alarm list
    next = *last;       // Points to the first alarm or head of linked list

    // Iterate through linked list
    while (next != NULL)
    {
        // If the current alarm's time is >= to the given alarm...
        if (next->alarm_id >= alarm->alarm_id)
        {
            // The new alarm's next alarm is set to the current alarm
            alarm->link = next;

            // The alarm prior is set to the alarm
            *last = alarm;
            // *last -> alarm -> next
            break;
        }
        // Next iteration
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

    printf("Alarm(%d) Inserted through Main Thread %lu Into Alarm List at %ld: Group(%d) %ld %s\n",
           alarm->alarm_id, pthread_self(), alarm->time, alarm->group_number, alarm->seconds, alarm->message);

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
        // Update global "current alarm"
        current_alarm = alarm->time;
        status = pthread_cond_signal(&alarm_cond);
        if (status != 0)
            err_abort(status, "Signal cond");
    }
}

/*
 * The alarm thread's start routine.
 */
void *alarm_thread(void *arg)
{
    alarm_t *alarm;
    struct timespec cond_time;
    time_t now;
    int status, expired;

    /*
     * Loop forever, processing commands. The alarm thread will
     * be disintegrated when the process exits. Lock the mutex
     * at the start -- it will be unlocked during condition
     * waits, so the main thread can insert alarms.
     */
    status = pthread_mutex_lock(&alarm_mutex);
    if (status != 0)
        err_abort(status, "Lock mutex");
    while (1)
    {
        /*
         * If the alarm list is empty, wait until an alarm is
         * added. Setting current_alarm to 0 informs the insert
         * routine that the thread is not busy.
         */
        current_alarm = 0;
        while (alarm_list == NULL)
        {
            // WAIT until alarm is added to linked list
            status = pthread_cond_wait(&alarm_cond, &alarm_mutex);
            // NOTE: cond_wait does three things
            // 1. pthread_mutex_unlock(&alarm_mutex)
            // 2. wait for signal on alarm_cond(from other threads)
            // 3. pthread_mutex_lock(&alarm_mutex) (after signal has been received)

            if (status != 0)
                err_abort(status, "Wait on cond");
        }

        alarm = alarm_list;       // 1. assign alarm to head of linked list
        alarm_list = alarm->link; // 2. move to the NEXT node in linked list (head = head.next)

        now = time(NULL); // Current time
        expired = 0;      // Not expired initially not expired

        // If alarm is not expired...
        if (alarm->time > now)
        {
#ifdef DEBUG
            printf("[waiting: %d(%d)\"%s\"]\n", alarm->time,
                   alarm->time - time(NULL), alarm->message);
#endif
            cond_time.tv_sec = alarm->time;
            cond_time.tv_nsec = 0;
            current_alarm = alarm->time;

            // While the alarm->time remains unchanged
            // Ex. Adding a new alarm would would break this equality
            while (current_alarm == alarm->time)
            {
                // Wait until alarm is expired
                // This signal can be triggered if?:
                // 1. A new alarm is added
                // 2. The alarm expires
                status = pthread_cond_timedwait(
                    &alarm_cond, &alarm_mutex, &cond_time);

                // When expired, set to expired and break out of loop
                if (status == ETIMEDOUT)
                {
                    expired = 1;
                    break;
                }

                // Any status other than ETIMEDOUT == an error in the program
                if (status != 0)
                    err_abort(status, "Cond timedwait");
            }

            // If not expired, insert the alarm back in to the linked list
            if (!expired)
                // Pass the pointer to the HEAD of the alarm list?
                alarm_insert(alarm);
        }

        // If alarm is expired..
        // 1. Change status to expired
        else
            expired = 1;

        // Deallocate space used by alarm
        if (expired)
        {
            printf("(%d) %s\n", alarm->seconds, alarm->message);
            free(alarm);
        }
    }
}

// Used to reduce redundancy
void handle_invalid_request()
{
    fprintf(stderr, "Invalid alarm request\n");
}

// Start_Alarm function
void start_alarm(int alarm_id, int group_number, int seconds, const char *message)
{

    // If characters > 128, truncate it to 128
    char truncated_message[129];
    strncpy(truncated_message, message, 128);
    truncated_message[128] = '\0';

    // Allocate memory for the new alarm
    alarm_t *alarm = (alarm_t *)malloc(sizeof(alarm_t));
    if (alarm == NULL)
        errno_abort("Allocate alarm");

    // Initialize the alarm
    alarm->link = NULL;
    alarm->seconds = seconds;
    alarm->time = time(NULL) + alarm->seconds;
    strncpy(alarm->message, message, sizeof(alarm->message) - 1);
    alarm->message[sizeof(alarm->message) - 1] = '\0';
    alarm->alarm_id = alarm_id;
    alarm->group_number = group_number;

    // Lock the mutex before inserting the alarm
    int status = pthread_mutex_lock(&alarm_mutex);
    if (status != 0)
        err_abort(status, "Lock mutex");

    // Insert the new alarm into the list
    alarm_insert(alarm);

    // Unlock the mutex after inserting the alarm
    status = pthread_mutex_unlock(&alarm_mutex);
    if (status != 0)
        err_abort(status, "Unlock mutex");
}

void change_alarm(int alarm_id, int group_number, int seconds, const char *message)
{
    // Lock the mutex before modifying the alarm
    int status = pthread_mutex_lock(&alarm_mutex);
    if (status != 0)
        err_abort(status, "Lock mutex");

    printf("alarm id: %d, group: %d, seconds: %d, message: %s\n", alarm_id, group_number, seconds, message);

    // // Find the alarm in the list and update its properties
    alarm_t *current = alarm_list;

    printf("%d : current_alarm", current_alarm);

    while (current != NULL)
    {
        printf("Looking for Alarm...");
        if (current->alarm_id == alarm_id && current->group_number == group_number)
        {
            current->seconds = seconds;
            current->time = time(NULL) + seconds;
            strncpy(current->message, message, sizeof(current->message) - 1);
            current->message[sizeof(current->message) - 1] = '\0';
            break;
        }
        current = current->link;
    }

    // // If no alarms or matching alarm does not exist, return error message
    if (current == NULL)
        printf("Alarm with id: %d and group_number: %d  does not exist.\n", alarm_id, group_number);

    // Unlock the mutex after modifying the alarm
    status = pthread_mutex_unlock(&alarm_mutex);
    if (status != 0)
        err_abort(status, "Unlock mutex");
}

int main(int argc, char *argv[])
{
    int status;
    char line[129];
    alarm_t *alarm;
    pthread_t thread;

    // The thread runs the alarm_thread function
    status = pthread_create(
        &thread, NULL, alarm_thread, NULL);
    if (status != 0)
        err_abort(status, "Create alarm thread");
    while (1)
    {
        printf("Alarm> ");

        // Allow user to exit
        if (fgets(line, sizeof(line), stdin) == NULL)
            exit(0);

        // If input is <=1 (blank or newline) ignore input
        if (strlen(line) <= 1)
            continue;

        // If Start_Alarm
        if (strncmp(line, "Start_Alarm", 11) == 0)
        {
            int seconds, alarm_id, group_number;
            char message[129];

            // Parse the Start_Alarm request and ensure all input are correct
            if (sscanf(line, "Start_Alarm(%d): Group(%d) %d %128[^\n]",
                       &alarm_id, &group_number, &seconds, message) < 4)
            {
                fprintf(stderr, "Faulty Start_Alarm request. Please try again\n");
                continue;
            }

            // If all inputs are valid, call the Start_Alarm function
            start_alarm(alarm_id, group_number, seconds, message);
        }

        else if (strncmp(line, "Change_Alarm", 11) == 0)
        {
            // Same setup as Start Alarm
            int alarm_id, group_number, seconds;
            char message[129];

            // Parse the Change_Alarm request
            if (sscanf(line, "Change_Alarm(%d): Group(%d) %d %128[^\n]",
                       &alarm_id, &group_number, &seconds, message) < 4)
            {
                fprintf(stderr, "Faulty Change_Alarm request. Please try again\n");
                continue;
            }

            // If all inputs are valid, call the change_alarm function
            change_alarm(alarm_id, group_number, seconds, message);
        }

        // Else invalid input
        else
        {
            handle_invalid_request();
        }
    }
}
