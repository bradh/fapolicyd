/*
 * lru.c - LRU cache implementation
 * Copyright (c) 2016 Red Hat Inc., Durham, North Carolina.
 * All Rights Reserved. 
 *
 * This software may be freely redistributed and/or modified under the
 * terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING. If not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor 
 * Boston, MA 02110-1335, USA.
 *
 * Authors:
 *   Steve Grubb <sgrubb@redhat.com>
 */

#include "config.h"
#include <stdlib.h>
#include <string.h>
#include "lru.h"
#include "message.h"

//#define DEBUG
 
// Local declarations
static void dequeue(Queue *queue);

// The Queue Node will store the 'item' being cached
static QNode *new_QNode(void)
{
	QNode *temp = malloc(sizeof(QNode));
	if (temp == NULL)
		return temp;
	temp->item = NULL;
	temp->uses = 1;	// Setting to 1 because its being used
 
	// Initialize prev and next as NULL
	temp->prev = temp->next = NULL;
 
	return temp;
}
 
static Hash *create_hash(unsigned int hsize)
{
	unsigned int i;

	Hash *hash = malloc(sizeof(Hash));
	if (hash == NULL)
		return hash;

	hash->array = malloc(hsize * sizeof(QNode*));
 
	// Initialize all hash entries as empty
	for (i = 0; i < hsize; i++)
		hash->array[i] = NULL;
 
	return hash;
}

static void destroy_hash(Hash *hash)
{
	free(hash->array);
	free(hash);
}
 
static void dump_queue_stats(const Queue *q)
{
	msg(LOG_DEBUG, "%s queue size: %u", q->name, q->total);
	msg(LOG_DEBUG, "%s slots in use: %u", q->name, q->count);
	msg(LOG_DEBUG, "%s hits: %lu", q->name, q->hits);
	msg(LOG_DEBUG, "%s misses: %lu", q->name, q->misses);
	msg(LOG_DEBUG, "%s evictions: %lu", q->name, q->evictions);
}

static Queue *create_queue(unsigned int qsize, const char *name)
{
	Queue *queue = malloc(sizeof(Queue));
	if (queue == NULL)
		return queue;
 
	// The queue is empty
	queue->count = 0;
	queue->hits = 0;
	queue->misses = 0;
	queue->evictions = 0;
	queue->front = queue->end = NULL;
 
	// Number of slots that can be stored in memory
	queue->total = qsize;

	queue->name = name;
 
	return queue;
}

static void destroy_queue(Queue *queue)
{
	dump_queue_stats(queue);

	while (queue->count)
		dequeue(queue);

	free(queue);
}
 
static unsigned int are_all_slots_full(const Queue *queue)
{
	return queue->count == queue->total;
}
 
static unsigned int queue_is_empty(const Queue *queue)
{
	return queue->end == NULL;
}

#ifdef DEBUG
static void sanity_check_queue(Queue *q, const char *id)
{
	unsigned int i;
	QNode *n = q->front;
	if (n == NULL)
		return;

	// Walk bottom to top
	i = 0;
	while (n->next) {
		if (n->next->prev != n) {
			msg(LOG_DEBUG, "%s - corruption found %u", id, i);
			abort();
		}
		if (i == q->count) {
			msg(LOG_DEBUG, "%s - forward loop found %u", id, i);
			abort();
		}
		i++;
		n = n->next;
	}

	// Walk top to bottom
	n = q->end;
	while (n->prev) {
		if (n->prev->next != n) {
			msg(LOG_DEBUG, "%s - Corruption found %u", id, i);
			abort();
		}
		if (i == 0) {
			msg(LOG_DEBUG, "%s - backward loop found %u", id, i);
			abort();
		}
		i--;
		n = n->prev;
	}
}
#else
#define sanity_check_queue(a, b) do {} while(0)
#endif

static void insert_before(Queue *queue, QNode *node, QNode *new_node)
{
	sanity_check_queue(queue, "1 insert_before");
	new_node->prev = node->prev;
	new_node->next  = node;
	if (node->prev == NULL)
		queue->front = new_node;
	else
		node->prev->next = new_node;
	node->prev = new_node;
	sanity_check_queue(queue, "2 insert_before");
}

static void insert_beginning(Queue *queue, QNode *new_node)
{
	sanity_check_queue(queue, "1 insert_beginning");
	if (queue->front == NULL) {
		queue->front = new_node;
		queue->end = new_node;
		new_node->prev = NULL;
		new_node->next = NULL;
	} else
		insert_before(queue, queue->front, new_node);
	sanity_check_queue(queue, "2 insert_beginning");
}

static void remove_node(Queue *queue, QNode *node)
{
	// If we are at the beginning
	sanity_check_queue(queue, "1 remove_node");
	if (node->prev == NULL) {
		queue->front = node->next;
		if (queue->front)
			queue->front->prev = NULL;
		goto out;
	} else {
		if (node->prev->next != node) {
			msg(LOG_ERR, "Linked list corruption detected %s",
				queue->name);
			abort();
		}
		node->prev->next = node->next;
	}

	// If we are at the end
	if (node->next == NULL) {
		queue->end = node->prev;
		if (queue->end)
			queue->end->next = NULL;
	} else {
		if (node->next->prev != node) {
			msg(LOG_ERR, "Linked List corruption detected %s",
				queue->name);
			abort();
		}
		node->next->prev = node->prev;
	}
out:
	sanity_check_queue(queue, "2 remove_node");
}

// Remove from the end of the queue 
static void dequeue(Queue *queue)
{
	QNode *temp = queue->end;

	if (queue_is_empty(queue))
		return;

	remove_node(queue, queue->end);

	queue->cleanup(temp->item); 
	free(temp->item);
	free(temp);
 
	// decrement the total of full slots by 1
	queue->count--;
}
 
// Remove front of the queue because its a mismatch
void lru_evict(Queue *queue, unsigned int key)
{
	Hash *hash = queue->hash;
	QNode *temp = queue->front;

	if (queue_is_empty(queue))
		return;
 
	hash->array[key] = NULL;
	remove_node(queue, queue->front);

	queue->cleanup(temp->item);
	free(temp->item);
	free(temp);

        // decrement the total of full slots by 1
	queue->count--;
	queue->evictions++;
}

// Make a new entry with item to be assigned later
// and setup the hash key
static void enqueue(Queue *queue, unsigned int key)
{
	QNode *temp;
	Hash *hash = queue->hash;

	// If all slots are full, remove the page at the end
	if (are_all_slots_full(queue)) {
		// remove page from hash
		hash->array[key] = NULL;
		dequeue(queue);
	}

	// Create a new node with given page total,
	// And add the new node to the front of queue
	temp = new_QNode();

	insert_beginning(queue, temp); 
	hash->array[key] = temp;
 
	// increment number of full slots
	queue->count++;
}
 
// This function is called needing an item from cache.
//  There are two scenarios:
// 1. Item is not in cache, so add it to the front of the queue
// 2. Item is in cache, we move the item to front of queue
QNode *check_lru_cache(Queue *queue, unsigned int key)
{
	QNode *reqPage;
	Hash *hash = queue->hash;

	// Check for out of bounds key
	if (key >= queue->total) {
		return NULL;
	}

	reqPage = hash->array[key];
 
	// item is not in cache, make new spot for it
	if (reqPage == NULL) {
		enqueue(queue, key);
		queue->misses++;
 
	// item is there but not at front. Move it
	} else if (reqPage != queue->front) {
		remove_node(queue, reqPage);
		reqPage->next = NULL; 
		reqPage->prev = NULL; 
		insert_beginning(queue, reqPage);
 
		// Increment cached object metrics
		queue->front->uses++;
		queue->hits++;
	} else
		queue->hits++;

	return queue->front;
}

Queue *init_lru(unsigned int qsize, void (*cleanup)(void *),
		const char *name)
{
	Queue *q = create_queue(qsize, name);
	if (q == NULL)
		return q;

	q->cleanup = cleanup;
	q->hash = create_hash(qsize);

	return q;
}

void destroy_lru(Queue *queue)
{
	if (queue == NULL)
		return;

	destroy_hash(queue->hash);
	destroy_queue(queue);
}

unsigned int compute_subject_key(const Queue *queue, unsigned int pid)
{
	if (queue)
		return pid % queue->total;
	else
		return 0;
}

unsigned long compute_object_key(const Queue *queue, unsigned long num)
{
	if (queue)
		return num % queue->total;
	else
		return 0;
}

