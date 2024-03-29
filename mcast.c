#include "mp1.h"

#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define RSEND_TIME 50 /* 50 milliseconds between ticks for RSEND */
#define RSEND_TICKS 10 /* wait 10 ticks of the rsend queue before trying again */
#define RSEND_TRIES 4 /* 4 attempts for rsend */
#define PING_TICKS 20
#define PING_TRIES 4
#define PING_INTERVAL 500 + PING_TICKS * PING_TRIES

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
    char msg_type;

} sendq_item;

/* Vector Clock, implemented as a singly linked list keeps a counter for
 * each peer. */
typedef struct vclock
{
	struct vclock *next;
	uint32_t time;
	uint16_t id;
} vclock_t;

/* causal ordered message queue for co_deliver */
typedef struct message_queue
{
	struct message_queue *next;
	vclock_t *timestamp;
	char *message;
	uint16_t source;
} message_queue_t;

/* Unicast sequence number and message sequence ID number */
unsigned int msg_sequence = 0;

/* thread for the sendq handler and the lock around the sendq */
pthread_mutex_t sendq_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_t r_send_thread;

/* unicast sendq array */
unsigned int sendq_length = 0;
unsigned int sendq_alloc = 0;
sendq_item **sendq_array;

typedef struct msg_record
{
    int source;
    unsigned int msg_id;
} msg_record_t;

/* duplicate protection */
unsigned int recbuf_length = 0;
unsigned int recbuf_alloc = 0;
msg_record_t *recbuf_array;
pthread_mutex_t recbuf_lock = PTHREAD_MUTEX_INITIALIZER;

/* Vector clock, holds sequence information for co_multicast messages */
vclock_t *local_clock = NULL;
pthread_mutex_t local_clock_lock;

/* Causal ordered message queue for co_deliver, holds recieved (but undelivered
 * messages in causal order) */
message_queue_t *causal_queue = NULL;
pthread_mutex_t causal_queue_lock;


int vclock_compare(vclock_t *a, vclock_t *b);

/* store number in base 36 in string, return the number of non-null characters
 * written */
size_t base36_encode(uint32_t number, char *string)
{
	size_t characters = 0;
	uint8_t i;
	uint8_t digit;
	char c;

	if (number == 0) {
		string[characters++] = '0';
	}

	while (number) {
		digit = number % 36;
		c = '\0';
		number = number / 36;

		if (digit < 10) {
			c = digit + 0x30;
		} else {
			c = digit - 10 + 0x41;
		}

		string[characters++] = c;
	}

	string[characters] = '\0'; // NULL terminate strings, but don't increment the count

	// reverse the string
	for (i = 0; i < characters / 2; ++i) {
		char c = string[characters - i - 1];
		string[characters - i - 1] = string[i];
		string[i] = c;
	}

	return characters;
}

/* returns 1 if id is a member of the mcast group, otherwise returns 0 */
int process_is_alive(uint16_t id)
{
	size_t i;

	assert(mcast_num_members >= 0);
	for (i = 0; i < (size_t)mcast_num_members; ++i) {
		if (mcast_members[i] == id) return 1;
	}

	return 0;
}

/* remove clock and all child nodes */
void vclock_free(vclock_t *clock)
{
	if (!clock) return;

	vclock_t *next = clock->next;
	free(clock);
	vclock_free(next);
}

/* remove the head of the causal_queue and advance the head to the next node */
void causal_queue_pop()
{
	message_queue_t *to_remove = causal_queue;
	causal_queue = causal_queue->next;

	vclock_free(to_remove->timestamp);
	free(to_remove->message);
	free(to_remove);
}

/* inserts a new message_queue entry at the correct sorted position in the queue and
 * copies the message content from message into the message_queue_t.message field */
void add_to_causal_queue(uint16_t source, vclock_t *timestamp, char *message)
{
	message_queue_t *entry   = (message_queue_t *)malloc(sizeof(message_queue_t));
	message_queue_t *current = causal_queue;

	assert(timestamp);
	assert(message);

	while (*(message++) != '!'); // advance message to start of message content

	entry->source    = source;
	entry->timestamp = timestamp;
	entry->message   = strdup(message);

#ifdef DEBUG
	printf("queued message from id: %d with message: %s with timestamp: [", source,
	       entry->message);
	while (timestamp) {
		printf(" %d: %d ", timestamp->id, timestamp->time);
		timestamp = timestamp->next;
	}
	printf("]\n");
#endif

	if (!current) {
		causal_queue = entry;
		entry->next = NULL;
		return;
	}

	if (vclock_compare(causal_queue->timestamp, entry->timestamp) >= 0) {
		entry->next = causal_queue;
		causal_queue = entry;
		return;
	}

	/* while not at the end of the queue and the timestamp of current->next is earlier
	 * than the timestamp advance */
	while (current->next && vclock_compare(current->next->timestamp, entry->timestamp) < 0) {
		current = current->next;
	}

	entry->next = current->next;
	current->next = entry;
}

/* create a new node with id */
vclock_t *vclock_new_node(uint32_t id)
{
	vclock_t *node = (vclock_t *)malloc(sizeof(vclock_t));
	node->id = id;
	node->time = 0;
	node->next = NULL;

	return node;
}

/* find's the node for id, inserts a new empty node if none exists */
vclock_t *vclock_find_id(vclock_t *clock, uint16_t id)
{
	assert(clock);

	if (clock->id == id) return clock;

	while (clock->next) {
		clock = clock->next;
		if (clock->id == id) return clock;
	}

	clock->next = vclock_new_node(id);

	return clock->next;
}

void vclock_insert(vclock_t *clock, vclock_t *node)
{
	assert(clock);
	assert(node->next == NULL);

	while (clock->next) {
		assert(clock->id != node->id);
		clock = clock->next;
	}

	clock->next = node;
}

int vclock_lock()
{
	return pthread_mutex_lock(&local_clock_lock);
}

int vclock_unlock()
{
	return pthread_mutex_unlock(&local_clock_lock);
}

/* returns negative if a precedes b, 0 if a is concurrent to b,
 * and positive if a succeeds b */
int vclock_compare(vclock_t *a, vclock_t *b)
{
	int difference = 0;
	vclock_t *a_current = a;
	vclock_t *b_current;

	assert(a);
	assert(b);

	while (a_current) {
		b_current = vclock_find_id(b, a_current->id);
		int distance = a_current->time - b_current->time;
		if (difference * distance < 0) {
			/* difference disagrees with distance */
			return 0;
		}

		difference += distance;
		a_current = a_current->next;
	}

	/* check for membership disagreement backwards as well */
	b_current = b;
	while (b_current) {
		a_current = vclock_find_id(a, b_current->id);
		int distance = a_current->time - b_current->time;
		if (difference * distance < 0) {
			return 0;
		}

		difference += distance;
		b_current = b_current->next;
	}

	return difference;
}

/* increment the time entry for id in clock, inserts new
 * clock entries if the id does not exist */
void vclock_increment(vclock_t *clock, uint16_t id)
{
	assert(clock);

	vclock_t *node = vclock_find_id(clock, id);

	++(node->time);
}

/* string representation of vector clock is: base64(id):base64(time):base64(id):... */
vclock_t *vclock_from_str(const char *str)
{
	vclock_t *head = NULL;
	vclock_t *tail = NULL;
	char current_36_id[5]   = "";
	char current_36_time[6] = "";
	char *current_field = current_36_id;
	char *other_field   = current_36_time;
	size_t offset = 0;

	while (*str || *str == '!') { // ! is end of vclock delmiter
		if (*str == ':') {
			if (current_field == current_36_id) {
				current_field = current_36_time;
			} else {
				current_field = current_36_id;
				uint16_t id = (uint16_t)strtol(current_36_id, NULL, 36);
				if (!tail) {
					head = tail = vclock_new_node(id);
				} else {
					assert(tail->next == NULL);
					tail->next = vclock_new_node(id);
					tail = tail->next;
				}
				tail->time = (uint32_t)strtol(current_36_time, NULL, 36);
			}
			offset = 0;
			++str;
			continue;
		}

		current_field[offset++] = *str;
		++str;
	}

	return head;
}

/* write a new string for a vclock */
char *vclock_to_str(const vclock_t *clock)
{
	char *str = (char *)malloc(10000); // max message size
	size_t offset = 0;

	while (clock) {
		offset += base36_encode(clock->id, str + offset);
		str[offset++] = ':';
		offset += base36_encode(clock->time, str + offset);
		str[offset++] = ':';
		clock = clock->next;
	}

	str[offset++] = '!'; // End of vclock delimiter
	str[offset] = '\0';
	return str;
}

/* reliable unicast send.  this fn wraps the unicast with a message
 * sequence number and will send it with reliabilty. */
void r_usend(int dest, const char *message, char msg_type)
{
    sendq_item *item;

    /* malloc the item and our message */
    item = (sendq_item*) malloc(sizeof(sendq_item));
    if (item == NULL)
    {
        fprintf(stderr, "Error: could not malloc for r_usend\n");
        return;
    }

    item->msg = (char*) calloc(sizeof(char), 16+strlen(message));
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
    item->msg_type = msg_type;

    /* prepare our outgoing message */
    snprintf(item->msg, 15+strlen(message), "%c:%u:%s", msg_type, msg_sequence, message);

    /* send message */
#ifdef DEBUG
    printf("%s\n", item->msg);
#endif
    if (dest != my_id)
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

    if (dest == my_id)
        receive(dest, message);

    return;
}

/* This function will remove pending pings from the send queue and will 
 * force more retries for the other items in the sendq */
void clear_pings(int source)
{
    int i;

    pthread_mutex_lock(&sendq_lock);

    for (i = 0; i < sendq_length; i++)
    {
	if (sendq_array[i]->dest == source)
	{
	    if (sendq_array[i]->msg_type == 'p')
	    {
		free(sendq_array[i]->msg);
		free(sendq_array[i]);
		sendq_length--;
		sendq_array[i] = sendq_array[sendq_length];
		i--;
	    }
	    else
	    {
		sendq_array[i]->n_sends = 1;
	    }
	}
    }
    pthread_mutex_unlock(&sendq_lock);
}

/* reliable multicast implementation.  uses a reliable unicast
 * to provide a reliable multicast.  */
void r_multicast(const char *message, char msg_type) {
#ifdef DEBUG
	static char *cached_message = NULL;
	static char cached_msg_type = '\0';
#endif
    int i;

    pthread_mutex_lock(&member_lock);

#ifdef DEBUG
    if (cached_message) {
#endif
	    /* send the current message */
	    for (i = 0; i < mcast_num_members; i++) {
		    r_usend(mcast_members[i], message, msg_type);
	    }
#ifdef DEBUG
	    /* send the previous message */
	    for (i = 0; i < mcast_num_members; i++) {
		    r_usend(mcast_members[i], cached_message, cached_msg_type);
	    }
	    free(cached_message);
	    cached_message = NULL;
    } else {
	    cached_message = strdup(message);
	    cached_msg_type = msg_type;
    }
#endif

    pthread_mutex_unlock(&member_lock);
}

void co_multicast(const char *message)
{
	char *clock_string;

	vclock_lock();
	vclock_increment(local_clock, my_id);
	clock_string = vclock_to_str(local_clock);
	vclock_unlock();

	strcat(clock_string, message); // XXX clock_string is assumed to be large enough
	r_multicast(clock_string, 'm');
	free(clock_string);
}

void co_deliver(uint16_t source, char *message)
{
	vclock_t *timestamp = vclock_from_str(message);

	pthread_mutex_lock(&causal_queue_lock);
	add_to_causal_queue(source, timestamp, message);
	assert(causal_queue);
	while (causal_queue) {
		vclock_lock();
		vclock_t *local_node  = vclock_find_id(local_clock,
		                                       causal_queue->source);
		vclock_t *remote_node = vclock_find_id(causal_queue->timestamp,
		                                       causal_queue->source);
		assert(remote_node->time > 0);
		if (local_node->time > remote_node->time) {
			/* duplicate message */
			causal_queue_pop();
		} else if (remote_node->time - local_node->time <= 1) {
			deliver(causal_queue->source, causal_queue->message);
			causal_queue_pop();
			local_node->time = remote_node->time;
		} else if (!process_is_alive(causal_queue->source)) {
			/* this message is waiting on a message from a failed process, we cannot
			 * guarantee that we'll ever recieve it so drop the message */
			causal_queue_pop();
		} else {
			vclock_unlock();
			break;
		}
		vclock_unlock();
	}
	pthread_mutex_unlock(&causal_queue_lock);
}

void multicast(const char *message)
{
    co_multicast(message);
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
#ifdef DEBUG
            printf("failing member: %d\n", member);
#endif
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
    int i, ping_counter = 0;
    int max_ticks, max_tries;

    r_multicast("", 'p');

    while (1)
    {
        if (ping_counter == PING_INTERVAL)
        {
            r_multicast("", 'p');
            ping_counter = 0;
        }
        else
        {
            ping_counter++;
        }

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
                switch (sendq_array[i]->msg_type)
                {
                    case 'p':
                        max_ticks = PING_TICKS;
                        max_tries = PING_TRIES;
                        break;
                    default:
                        max_ticks = RSEND_TICKS;
                        max_tries = RSEND_TRIES;
                        break;
                }

                if (sendq_array[i]->n_ticks < max_ticks)
                {
                    sendq_array[i]->n_ticks++;
                }
                else
                {
                    if (sendq_array[i]->n_sends < max_tries)
                    {
                        usend(sendq_array[i]->dest, sendq_array[i]->msg);
                        sendq_array[i]->n_sends++;
                        sendq_array[i]->n_ticks = 0;
                    }
                    else
                    {
                        if (sendq_array[i]->dest != my_id)
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

/* exit handler for cleanup */
void exit_handler(void)
{
    r_multicast("", 'x');

    if (mcast_num_members == 1)
    {
        unlink(GROUP_FILE);
    }
}

void multicast_init(void) {
    unicast_init();

    /* malloc an array for the send queue and create a thread
     * for the queue processing */
    sendq_alloc = 1000;
    sendq_array = NULL;
    sendq_array = malloc(sendq_alloc * sizeof(sendq_item*));

    /* malloc an array for a list of message IDs that have been recv'd
     * so we don't get duplicates */
    recbuf_alloc = 1000;
    recbuf_array = NULL;
    recbuf_array = malloc(recbuf_alloc * sizeof(int));

    /* initialize the local_clock */
    pthread_mutex_init(&local_clock_lock, NULL);
    local_clock = vclock_new_node(my_id);

    /* initialize the causal_queue */
    pthread_mutex_init(&causal_queue_lock, NULL);
    causal_queue = NULL;

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

    atexit(exit_handler);

    r_multicast("", 's');
}

/* receive callback */
void receive(int source, const char *message) {

    char mode = '\0';
    char *msg_ptr = (char *)message;
    char msg_resp[512];
    unsigned int msg_id, i;
    vclock_t *clock_i;

    /* get the message type from the first character */
    mode = message[0];

    clear_pings(source);
    
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

	    for (i = 0; i < recbuf_length; i++)
	    {
		if (recbuf_array[i].msg_id == msg_id && recbuf_array[i].source == source)
		    break;
	    }

	    if (i == recbuf_length)
	    {
		/* deliver the message to the next stage */
                co_deliver(source, msg_ptr);

		pthread_mutex_lock(&recbuf_lock);
		if (recbuf_length == recbuf_alloc)
		{
		    recbuf_alloc *= 2;
		    recbuf_array = realloc(recbuf_array, recbuf_alloc * sizeof(msg_record_t));
		    if (recbuf_array == NULL) 
		    {
			perror("recbuf realloc failed");
			exit(1);
		    }
		}
		recbuf_array[recbuf_length].msg_id = msg_id;
		recbuf_array[recbuf_length].source = source;
		pthread_mutex_unlock(&recbuf_lock);
	    }
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

	/* ping message.  Send an ack back */
        case 'p':
            msg_ptr += 2;

            msg_id = atoi(msg_ptr);

            snprintf(msg_resp, 511, "a:%u", msg_id);
            usend(source, msg_resp);

            break;
	
	/* sync message.  send what our clock is, zero their clock */
	case 's':
	    msg_ptr += 2;

	    msg_id = atoi(msg_ptr);

	    snprintf(msg_resp, 511, "a:%u", msg_id);
	    usend(source, msg_resp);

	    vclock_lock();
	    clock_i = vclock_find_id(local_clock, source);
	    clock_i->time = 0;
	    clock_i = vclock_find_id(local_clock, my_id);
	    snprintf(msg_resp, 511, "%u", clock_i->time);
	    r_usend(source, msg_resp, 'y');
	    vclock_unlock();

	    break;

	/* sync reply, add their clock onto our clock queue */
	case 'y':
	    msg_ptr += 2;
	    msg_id = atoi(msg_ptr);
	    snprintf(msg_resp, 511, "a:%u", msg_id);
	    usend(source, msg_resp);

	    while (*(msg_ptr++) != ':');

	    vclock_lock();
	    clock_i = vclock_find_id(local_clock, source);
	    clock_i->time = atoi(msg_ptr);
	    vclock_unlock();
	    break;

	/* member exited, fail them from the list */
	case 'x':
	    fail_member(source);
	    break;

        default:
            break;

    }

}


