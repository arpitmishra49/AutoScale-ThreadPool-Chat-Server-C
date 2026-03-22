/* =========================================================
 * thread_pool.c — Implementation of the autoscaling thread pool
 * =========================================================
 *
 * Read thread_pool.h first for the full design rationale.
 * This file contains every function that creates, runs, and
 * destroys the pool.
 * ========================================================= */

#include "thread_pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>     /* ETIMEDOUT — returned by pthread_cond_timedwait on deadline */
#include <time.h>      /* clock_gettime(), struct timespec, CLOCK_REALTIME           */

/* ── Internal helpers (not exposed in the header) ─────────── */

/*
 * linked_list_insert_at_head()
 *   Inserts `node` at the front of the active doubly-linked list
 *   that lives inside the pool.  O(1) operation.
 *
 *   Before:  head → [A] ↔ [B] ↔ NULL
 *   After:   head → [node] ↔ [A] ↔ [B] ↔ NULL
 */
static void linked_list_insert_at_head(thread_pool_t *pool,
                                       thread_node_t *node)
{
    /* Point new node's next to the current head */
    node->next = pool->active_head;

    /* New node has no predecessor at the head */
    node->prev = NULL;

    /* If the list was non-empty, fix the old head's prev pointer */
    if (pool->active_head != NULL) {
        pool->active_head->prev = node;
    }

    /* Advance the head pointer to the new node */
    pool->active_head = node;

    /* Increment the count of active workers */
    pool->active_count++;
}

/*
 * linked_list_remove_node()
 *   Unlinks `node` from wherever it sits in the doubly-linked
 *   list.  O(1) — no scan needed because we have both pointers.
 *
 *   Handles three cases:
 *     1. Node is the head (prev == NULL)
 *     2. Node is the tail (next == NULL)
 *     3. Node is in the middle
 */
static void linked_list_remove_node(thread_pool_t *pool,
                                    thread_node_t *node)
{
    /* Stitch the predecessor's next past this node */
    if (node->prev != NULL) {
        node->prev->next = node->next;
    } else {
        /* This node was the head; move head forward */
        pool->active_head = node->next;
    }

    /* Stitch the successor's prev past this node */
    if (node->next != NULL) {
        node->next->prev = node->prev;
    }

    /* Poison the pointers so dangling-pointer bugs are obvious */
    node->prev = NULL;
    node->next = NULL;

    pool->active_count--;
}

/*
 * dequeue_oldest_task()
 *   Removes and returns the head of the FIFO task queue.
 *   Caller MUST hold pool_mutex.
 *   Returns NULL if the queue is empty.
 */
static task_node_t *dequeue_oldest_task(thread_pool_t *pool)
{
    if (pool->task_queue_head == NULL) {
        return NULL;
    }

    /* Grab the front node */
    task_node_t *task = pool->task_queue_head;

    /* Advance the head pointer */
    pool->task_queue_head = task->next;

    /* If we just emptied the queue, tail must also become NULL */
    if (pool->task_queue_head == NULL) {
        pool->task_queue_tail = NULL;
    }

    pool->pending_task_count--;
    return task;
}

/*
 * enqueue_new_task()
 *   Appends a task to the tail of the FIFO queue.
 *   Caller MUST hold pool_mutex.
 */
static void enqueue_new_task(thread_pool_t   *pool,
                             task_function_t  function,
                             void            *argument)
{
    /* Allocate a task node on the heap */
    task_node_t *task = malloc(sizeof(task_node_t));
    if (task == NULL) {
        perror("enqueue_new_task: malloc failed");
        return;
    }

    task->execute   = function;
    task->argument  = argument;
    task->next      = NULL; /* it will be the new tail */

    if (pool->task_queue_tail == NULL) {
        /* Queue was empty: both head and tail point to the new task */
        pool->task_queue_head = task;
        pool->task_queue_tail = task;
    } else {
        /* Append after current tail, then advance tail */
        pool->task_queue_tail->next = task;
        pool->task_queue_tail       = task;
    }

    pool->pending_task_count++;
}

/*
 * initialise_worker_node()
 *   Zeroes and fills in the metadata fields of a thread_node_t
 *   at slot index `slot_index` in the pool's worker_array[].
 *   Does NOT start the thread yet.
 */
static void initialise_worker_node(thread_pool_t *pool,
                                   int            slot_index)
{
    thread_node_t *node = &pool->worker_array[slot_index];

    memset(node, 0, sizeof(thread_node_t));

    node->slot_index = slot_index;
    node->is_active  = 0;      /* not yet linked into the active list */
    node->prev       = NULL;
    node->next       = NULL;
    node->pool       = pool;   /* back-pointer used inside the worker function */
}

/* ── Worker thread entry point ────────────────────────────── */

/*
 * build_idle_timeout_deadline()
 *   Fills `deadline` with the current wall-clock time plus
 *   IDLE_TIMEOUT_SECONDS.  Passed to pthread_cond_timedwait()
 *   so workers do not sleep forever when the queue is empty.
 */
static void build_idle_timeout_deadline(struct timespec *deadline)
{
    /*
     * CLOCK_REALTIME is the only clock guaranteed to work with
     * pthread_cond_timedwait() on all POSIX platforms.
     */
    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_sec += IDLE_TIMEOUT_SECONDS;
}

/*
 * worker_thread_loop()
 *   The function every worker thread runs from creation until
 *   it scales down or the pool is destroyed.
 *
 *   Lifecycle per iteration:
 *     1. Lock the pool mutex.
 *     2. Wait on task_available_cond with a deadline.
 *          • Woken early  → a task arrived or shutdown was requested.
 *          • Timed out    → idle too long; check scale-down rule.
 *     3. Scale-down check (only on timeout):
 *          If queue still empty AND active_count > min_threads
 *          → unlink self, mark slot free, exit the thread.
 *     4. Shutdown check: exit if shutdown_flag set and queue empty.
 *     5. Dequeue one task, unlock, execute it outside the lock.
 *     6. Re-lock, broadcast all_tasks_done_cond if queue empty.
 *     7. Repeat from step 1.
 *
 *   `arg` is a pointer to this thread's own thread_node_t.
 */
static void *worker_thread_loop(void *arg)
{
    thread_node_t *self = (thread_node_t *)arg;
    thread_pool_t *pool = self->pool;

    while (1) {
        /* ── 1. Lock before touching any shared state ────── */
        pthread_mutex_lock(&pool->pool_mutex);

        /*
         * ── 2. Timed wait while queue is empty ────────────
         *
         * We use pthread_cond_timedwait() instead of the plain
         * pthread_cond_wait() so that workers automatically wake
         * up after IDLE_TIMEOUT_SECONDS even if no task arrives.
         * That wake-up is what enables scale-down.
         *
         * pthread_cond_timedwait() atomically:
         *   (a) releases pool_mutex
         *   (b) suspends this thread until signalled OR deadline
         * On return it re-acquires pool_mutex before returning.
         *
         * Return value:
         *   0          → woken by a signal (task available or shutdown)
         *   ETIMEDOUT  → deadline elapsed with no signal
         */
        int wait_result = 0;

        while (pool->pending_task_count == 0 && !pool->shutdown_flag) {
            struct timespec deadline;
            build_idle_timeout_deadline(&deadline);

            wait_result = pthread_cond_timedwait(&pool->task_available_cond,
                                                  &pool->pool_mutex,
                                                  &deadline);

            /*
             * Break out of the inner while as soon as we time out so
             * we can evaluate the scale-down rule below.
             * If we were woken early (wait_result == 0) the outer
             * while condition re-checks pending_task_count normally.
             */
            if (wait_result == ETIMEDOUT) {
                break;
            }
        }

        /*
         * ── 3. Scale-down check ────────────────────────────
         *
         * Conditions that must ALL be true to scale down:
         *   a) We woke due to a timeout (not a real task signal).
         *   b) The queue is still empty (no task snuck in).
         *   c) We are above the minimum thread floor.
         *   d) The pool is not shutting down (destroy handles that).
         *
         * When we scale down we:
         *   • Remove this node from the active doubly-linked list (O(1)).
         *   • Mark the array slot as free (is_active = 0) so it can be
         *     reused by a future spawn_worker_into_slot() call.
         *   • Release the mutex and exit the thread.
         */
        if (wait_result == ETIMEDOUT
                && pool->pending_task_count == 0
                && pool->active_count > pool->min_threads
                && !pool->shutdown_flag)
        {
            printf("[ThreadPool] Scaling down: %d -> %d workers "
                   "(idle timeout, min=%d)\n",
                   pool->active_count, pool->active_count - 1,
                   pool->min_threads);

            /* Unlink from active doubly-linked list */
            linked_list_remove_node(pool, self);

            /* Free the slot so it can be reused */
            self->is_active = 0;

            pthread_mutex_unlock(&pool->pool_mutex);
            pthread_exit(NULL);   /* this thread is done */
        }

        /* ── 4. Shutdown check ───────────────────────────── */
        if (pool->shutdown_flag && pool->pending_task_count == 0) {
            linked_list_remove_node(pool, self);
            self->is_active = 0;
            pthread_mutex_unlock(&pool->pool_mutex);
            pthread_exit(NULL);
        }

        /* ── 5. Dequeue one task and release the lock ─────── */
        task_node_t *task = dequeue_oldest_task(pool);

        pthread_mutex_unlock(&pool->pool_mutex);

        /* Execute the task completely outside the lock so other
         * workers can dequeue their own tasks concurrently.       */
        if (task != NULL) {
            task->execute(task->argument);
            free(task);
        }

        /* ── 6. Re-lock to signal idle waiters if needed ─── */
        pthread_mutex_lock(&pool->pool_mutex);

        /*
         * If the queue is now empty, wake anyone sleeping in
         * thread_pool_wait_until_idle().
         */
        if (pool->pending_task_count == 0) {
            pthread_cond_broadcast(&pool->all_tasks_done_cond);
        }

        pthread_mutex_unlock(&pool->pool_mutex);

        /* ── 7. Loop back to step 1 ──────────────────────── */
    }

    return NULL; /* never reached */
}

/* ── Spawning a new worker ────────────────────────────────── */

/*
 * spawn_worker_into_slot()
 *   Finds a free slot in worker_array[] (is_active == 0),
 *   initialises the node, starts the POSIX thread, and inserts
 *   the node into the active linked list.
 *
 *   Caller MUST hold pool_mutex.
 *   Returns 0 on success, -1 if no free slot exists or if
 *   pthread_create() fails.
 */
static int spawn_worker_into_slot(thread_pool_t *pool)
{
    /* Find a free (inactive) slot in the array */
    int free_slot = -1;
    for (int i = 0; i < pool->capacity; i++) {
        if (!pool->worker_array[i].is_active) {
            free_slot = i;
            break;
        }
    }

    if (free_slot == -1) {
        /* No free slot; caller should have grown the array first */
        return -1;
    }

    /* Initialise the node metadata */
    initialise_worker_node(pool, free_slot);

    thread_node_t *node = &pool->worker_array[free_slot];
    node->is_active = 1;

    /* Link into the active doubly-linked list */
    linked_list_insert_at_head(pool, node);

    /* Start the POSIX thread; it will run worker_thread_loop() */
    int create_result = pthread_create(&node->thread_id,
                                       NULL,
                                       worker_thread_loop,
                                       node);
    if (create_result != 0) {
        fprintf(stderr, "spawn_worker_into_slot: pthread_create failed\n");
        /* Undo the list insertion */
        linked_list_remove_node(pool, node);
        node->is_active = 0;
        return -1;
    }

    /* Detach: we will not join workers individually */
    pthread_detach(node->thread_id);

    return 0;
}

/*
 * grow_worker_array_and_spawn()
 *   Doubles the capacity of worker_array[] via realloc, then
 *   spawns one new worker in the fresh space.
 *   This is called by thread_pool_submit_task() when all slots
 *   are occupied and active_count < max_threads.
 *
 *   Caller MUST hold pool_mutex.
 *   Returns 0 on success, -1 on failure.
 */
static int grow_worker_array_and_spawn(thread_pool_t *pool)
{
    int new_capacity = pool->capacity * 2;

    thread_node_t *new_array = realloc(pool->worker_array,
                                       new_capacity * sizeof(thread_node_t));
    if (new_array == NULL) {
        perror("grow_worker_array_and_spawn: realloc failed");
        return -1;
    }

    /*
     * IMPORTANT: realloc may have moved the array to a new address.
     * All back-pointers from the linked list nodes now point into
     * the old (freed) memory.  We must re-walk the linked list and
     * fix every node's pool pointer, and also fix every prev/next
     * pointer to reference addresses inside new_array[].
     *
     * Strategy:
     *   Each node stores its slot_index.  After realloc we can
     *   recalculate the correct address as &new_array[slot_index].
     */
    pool->worker_array = new_array;

    /* Re-initialise the new (unused) slots */
    for (int i = pool->capacity; i < new_capacity; i++) {
        memset(&pool->worker_array[i], 0, sizeof(thread_node_t));
        pool->worker_array[i].pool = pool;
    }

    /*
     * After realloc the old pointer values inside prev/next are invalid.
     * Safest fix: rebuild the linked list entirely by scanning
     * new_array[] for all active slots using their slot_index.
     */
    pool->active_head  = NULL;
    pool->active_count = 0;

    for (int i = 0; i < pool->capacity; i++) { /* only old slots */
        if (pool->worker_array[i].is_active) {
            pool->worker_array[i].pool = pool;
            pool->worker_array[i].prev = NULL;
            pool->worker_array[i].next = NULL;
            linked_list_insert_at_head(pool, &pool->worker_array[i]);
        }
    }

    /* Update capacity */
    pool->capacity = new_capacity;

    /* Now spawn one new worker in the expanded space */
    return spawn_worker_into_slot(pool);
}

/* ── Public API implementation ────────────────────────────── */

/*
 * thread_pool_create()
 *   See thread_pool.h for the contract.
 */
thread_pool_t *thread_pool_create(int initial_threads,
                                  int min_threads,
                                  int max_threads)
{
    if (initial_threads <= 0 || min_threads <= 0 ||
        max_threads < initial_threads) {
        fprintf(stderr, "thread_pool_create: invalid arguments\n");
        return NULL;
    }

    /* Allocate the pool struct */
    thread_pool_t *pool = calloc(1, sizeof(thread_pool_t));
    if (pool == NULL) {
        perror("thread_pool_create: calloc pool");
        return NULL;
    }

    /* Set scaling limits */
    pool->min_threads = min_threads;
    pool->max_threads = max_threads;
    pool->capacity    = initial_threads * 2; /* start with headroom */

    /* Allocate the worker array */
    pool->worker_array = calloc(pool->capacity, sizeof(thread_node_t));
    if (pool->worker_array == NULL) {
        perror("thread_pool_create: calloc worker_array");
        free(pool);
        return NULL;
    }

    /* Set each node's back-pointer to the pool */
    for (int i = 0; i < pool->capacity; i++) {
        pool->worker_array[i].pool = pool;
    }

    /* Initialise synchronisation primitives */
    pthread_mutex_init(&pool->pool_mutex,         NULL);
    pthread_cond_init (&pool->task_available_cond, NULL);
    pthread_cond_init (&pool->all_tasks_done_cond, NULL);

    /* Spawn the initial workers */
    pthread_mutex_lock(&pool->pool_mutex);
    for (int i = 0; i < initial_threads; i++) {
        if (spawn_worker_into_slot(pool) != 0) {
            fprintf(stderr, "thread_pool_create: failed to spawn worker %d\n", i);
        }
    }
    pthread_mutex_unlock(&pool->pool_mutex);

    printf("[ThreadPool] Created with %d workers (min=%d, max=%d, capacity=%d)\n",
           initial_threads, min_threads, max_threads, pool->capacity);

    return pool;
}

/*
 * thread_pool_submit_task()
 *   See thread_pool.h for the contract.
 */
int thread_pool_submit_task(thread_pool_t   *pool,
                            task_function_t  function,
                            void            *argument)
{
    if (pool == NULL || function == NULL) {
        return -1;
    }

    pthread_mutex_lock(&pool->pool_mutex);

    if (pool->shutdown_flag) {
        pthread_mutex_unlock(&pool->pool_mutex);
        fprintf(stderr, "thread_pool_submit_task: pool is shutting down\n");
        return -1;
    }

    /* Enqueue the task */
    enqueue_new_task(pool, function, argument);

    /*
     * Autoscale UP: if all current workers are busy (pending tasks
     * exceeds idle capacity) and we have not hit max_threads, grow.
     *
     * Heuristic: scale up if pending tasks > 0 and all slots busy.
     * A more sophisticated policy would track per-worker idle state.
     */
    int all_workers_busy = (pool->pending_task_count > pool->active_count);

    if (all_workers_busy && pool->active_count < pool->max_threads) {
        printf("[ThreadPool] Autoscaling up: %d -> %d workers\n",
               pool->active_count, pool->active_count + 1);

        if (pool->active_count >= pool->capacity) {
            /* Need more array space */
            grow_worker_array_and_spawn(pool);
        } else {
            spawn_worker_into_slot(pool);
        }
    }

    /* Wake one sleeping worker to handle the new task */
    pthread_cond_signal(&pool->task_available_cond);

    pthread_mutex_unlock(&pool->pool_mutex);
    return 0;
}

/*
 * thread_pool_wait_until_idle()
 *   See thread_pool.h for the contract.
 */
void thread_pool_wait_until_idle(thread_pool_t *pool)
{
    pthread_mutex_lock(&pool->pool_mutex);

    while (pool->pending_task_count > 0) {
        pthread_cond_wait(&pool->all_tasks_done_cond, &pool->pool_mutex);
    }

    pthread_mutex_unlock(&pool->pool_mutex);
}

/*
 * thread_pool_destroy()
 *   See thread_pool.h for the contract.
 */
void thread_pool_destroy(thread_pool_t *pool)
{
    if (pool == NULL) return;

    /* Signal all workers to stop after finishing queued tasks */
    pthread_mutex_lock(&pool->pool_mutex);
    pool->shutdown_flag = 1;
    pthread_cond_broadcast(&pool->task_available_cond); /* wake all sleepers */
    pthread_mutex_unlock(&pool->pool_mutex);

    /* Give workers time to exit gracefully */
    /* (They are detached so we just sleep briefly instead of joining) */
    struct timespec wait_time = { .tv_sec = 1, .tv_nsec = 0 };
    nanosleep(&wait_time, NULL);

    /* Free the task queue (any tasks that were never consumed) */
    pthread_mutex_lock(&pool->pool_mutex);
    task_node_t *current_task = pool->task_queue_head;
    while (current_task != NULL) {
        task_node_t *next_task = current_task->next;
        free(current_task);
        current_task = next_task;
    }
    pthread_mutex_unlock(&pool->pool_mutex);

    /* Destroy synchronisation primitives */
    pthread_mutex_destroy(&pool->pool_mutex);
    pthread_cond_destroy (&pool->task_available_cond);
    pthread_cond_destroy (&pool->all_tasks_done_cond);

    /* Free the worker array and pool struct */
    free(pool->worker_array);
    free(pool);

    printf("[ThreadPool] Destroyed.\n");
}