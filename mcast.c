#include "mp1.h"

#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RSEND_TIME 50 /* 50 milliseconds between ticks for RSEND */
#define RSEND_TICKS 10 /* wait 10 ticks of the rsend queue before trying again */
#define RSEND_TRIES 4 /* 4 attempts for rsend */

/* Structure for a item in the send queue.  We keep
 * a message, the message id, the number of send attempts,
 * the number of ticks since last send and linked list pointers */
typedef struct 
{
    char *msg;
    unsigned int msg_id;
    int dest;
    unsigned int n_sends;
    unsigned int n_ticks;

} sendq_item;

/* Vector Clock, implemented as a singly linked list keeps a counter for
 * each peer. */
typedef struct vclock
{
	uint32_t id;
	uint32_t time;
	struct vclock *next;
} vclock_t;

/* Unicast sequence number and message sequence ID number */
unsigned int msg_sequence = 0;

/* thread for the sendq handler and the lock around the sendq */
pthread_mutex_t sendq_lock;
pthread_t r_send_thread;

/* unicast sendq array */
unsigned int sendq_length = 0;
unsigned int sendq_alloc = 0;

sendq_item **sendq_array;

vclock_t *vclock_find_id(uint32_t id, vclock_t *clock)
{
	while (clock) {
		if (id == clock->id) return clock;
		clock = clock->next;
	}

	return NULL;
}

/* returns negative if a precedes b, 0 if a is concurrent to b,
 * and positive if a succeeds b */
int vclock_compare(vclock_t *a, vclock_t *b)
{
	int difference = 0;
	vclock_t *a_current = a;
	vclock_t *b_current;

	while (a_current) {
		b_current = vclock_find_id(a_current->id, b);
		if (b_current) {
			int distance = a_current->time - b_current->time;
			/* if there is a difference we are concurrent if any of the
			 * timestamps does not agree with that difference */
			if (difference < 0 && distance > 0) {
				return 0;
			}
			if (difference > 0 && distance < 0) {
				return 0;
			}
			difference += distance;
		} else {
			/* the two vector clocks disagree on membership, as membership only
			 * increases a must not have ocurred before b. */
			if (difference < 0) return 0;
			difference += a_current->time;
		}

		a_current = a_current->next;
	}

	/* check for membership disagreement backwards as well */
	b_current = b;
	while (b_current) {
		if (!vclock_find_id(b_current->id, a)) {
			/* the two vector clocks disagree on membership, as membership only
			 * increases b must not have occurred before a. */
			if (difference > 0) return 0;
			difference -= b_current->time;
		}

		b_current = b_current->next;
	}

	return difference;
}

/* reliable unicast send.  this fn wraps the unicast with a message
 * sequence number and will send it with reliabilty. */
void r_usend(int dest, const char *message)
{
    sendq_item *item;

    /* malloc the item and our message */
    item = (sendq_item*) malloc(sizeof(sendq_item));
    if (item == NULL)
    {
        fprintf(stderr, "Error: could not malloc for r_usend\n");
        return;
    }

    item->msg = (char*) calloc(sizeof(char), 64+strlen(message));
    if (item->msg == NULL)
    {
        fprintf(stderr, "Error: could not malloc for r_usend\n");
        free(item);
        return;
    }

    /* lock around the send and the list operations */
    pthread_mutex_lock(&sendq_lock);

    /* initalize our item */
    msg_sequence++;
    item->msg_id = msg_sequence;
    item->dest = dest;
    item->n_sends = 1;
    item->n_ticks = 0;

    /* prepare our outgoing message */
    snprintf(item->msg, 63+strlen(message), "m:%u:%s", msg_sequence, message);

    /* send message */
    usend(dest, item->msg);

    if (sendq_length == sendq_alloc)
    {
        sendq_alloc *= 2;
        sendq_array = realloc(sendq_array, sendq_alloc * sizeof(sendq_item*));

        if (sendq_array == NULL)
        {
            fprintf(stderr, "Could not reallocate memory for queue\n");
            exit(1);
        }
    }

    sendq_array[sendq_length++] = item;

    pthread_mutex_unlock(&sendq_lock);

    return;
}

/* Basic multicast implementation */
void multicast(const char *message) {
    int i;
    
    pthread_mutex_lock(&member_lock);
    for (i = 0; i < mcast_num_members; i++) {
        r_usend(mcast_members[i], message);
    }
    pthread_mutex_unlock(&member_lock);
}

void co_multicast(const char *message)
{
	
}

/* Fail a member in our list, remove it and tell everyone
 * else that we've failed this member */
int fail_member(int member)
{
    int i, found_members = 0;

    /* lock and iterate over the list */
    pthread_mutex_lock(&member_lock);
    for (i = 0; i < mcast_num_members; i++)
    {
        /* if we find the member, increase our count of found members
         * and replace this entry with the one at the tail of the 
         * array. */
        if (member == mcast_members[i])
        {
            found_members++;
            mcast_members[i] = mcast_members[mcast_num_members - found_members];
            printf("failing member: %d\n", member);
        }
    }
    mcast_num_members -= found_members;
    pthread_mutex_unlock(&member_lock);

    /* TODO announce the failure to all other members */

    return found_members;
}

/* thread for handling resending un-acked unicast messages */
void *r_send_thread_main(void *discard)
{
    int i;

    while (1)
    {
        /* sleep for specified time */
        usleep( RSEND_TIME * 1000 );

        /* iterate the send queue and check to see where they are
         * in the process.  if they've gone through enough ticks
         * we will retry and if there have been too many retries,
         * we will fail the process. */
        pthread_mutex_lock(&sendq_lock);
        if (sendq_length > 0)
        {
            for (i = 0; i < sendq_length; i++)
            {
                if (sendq_array[i]->n_ticks < RSEND_TICKS)
                {
                    sendq_array[i]->n_ticks++;
                }
                else
                {
                    if (sendq_array[i]->n_sends < RSEND_TRIES)
                    {
                        usend(sendq_array[i]->dest, sendq_array[i]->msg);
                        sendq_array[i]->n_sends++;
                        sendq_array[i]->n_ticks = 0;
                    }
                    else
                    {
                        fail_member(sendq_array[i]->dest);
                        free(sendq_array[i]->msg);
                        free(sendq_array[i]);
                        sendq_array[i] = sendq_array[sendq_length - 1];
                        sendq_length--;
                        i--;
                    }
                }
            }
        }
        pthread_mutex_unlock(&sendq_lock);

    }

    return NULL;

}


void multicast_init(void) {
    unicast_init();

    /* malloc an array for the send queue and create a thread
     * for the queue processing */
    sendq_alloc = 1000;
    sendq_array = NULL;
    sendq_array = malloc(sendq_alloc * sizeof(sendq_item*));

    if (sendq_array == NULL)
    {
        fprintf(stderr, "Could not allocate memory for the sendq array\n");
        exit(1);
    }

    if (pthread_create(&r_send_thread, NULL, &r_send_thread_main, NULL) != 0)
    {
        fprintf(stderr, "Could not create worker thread for r_usend\n");
        exit(1);
    }
}

/* receive callback */
void receive(int source, const char *message) {

    char mode = '\0';
    char *msg_ptr = (char *)message;
    char msg_resp[512];
    unsigned int msg_id, i;

    /* get the message type from the first character */
    mode = message[0];
    
    switch (mode)
    {
        /* standard message */
        case 'm':
            /* skip past the type and delimiter */
            msg_ptr += 2;

            /* get the message ID number */
            msg_id = atoi(msg_ptr);

            /* skip past the second delimiter */
            while (msg_ptr[0] != ':')
            {
                if (msg_ptr[0] == '\0')
                    break;

                msg_ptr++;
            }

            if (msg_ptr[0] == ':')
                msg_ptr++;

            /* send the reply via unreliable unicast */
            snprintf(msg_resp, 511, "a:%u", msg_id);
            usend(source, msg_resp);

            /* deliver the message to the next stage */
            deliver(source, msg_ptr);
            break;

        /* message ack */
        case 'a':
            /* skip past type and delimiter */
            msg_ptr += 2;

            /* get the message ID */
            msg_id = atoi(msg_ptr);

            pthread_mutex_lock(&sendq_lock);

            /* remove the entry in the send queue, if it exists */
            for (i = 0; i < sendq_length; i++)
            {
                if (msg_id == sendq_array[i]->msg_id && source == sendq_array[i]->dest)
                {
                    free(sendq_array[i]->msg);
                    free(sendq_array[i]);
                    sendq_array[i] = sendq_array[sendq_length - 1];
                    sendq_length--;
                }
            }
            pthread_mutex_unlock(&sendq_lock);
            break;

        default:
            break;

    }

}

