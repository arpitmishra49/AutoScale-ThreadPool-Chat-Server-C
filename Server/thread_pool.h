#ifndef THREAD_POOL_H
#define THREAD_POOL_H

/* =========================================================
 * thread_pool.h — Thread pool with autoscaling
 * =========================================================
 *
 * DATA STRUCTURE RATIONALE
 * ─────────────────────────
 * Each "slot" in the pool is a thread_node_t.
 * Thread nodes are stored in a flat array (thread_node_t[])
 * BUT each node also carries next/prev pointers so they form
 * an intrusive doubly-linked list of ACTIVE nodes inside that
 * same array.
 *
 *   Array index:  [0]   [1]   [2]   [3]   [4]
 *                  │     │           │
 *               active active      active   ← linked list skips [2] (idle/free)
 *
 * WHY THIS HYBRID?
 *   • O(1) direct slot access by index (array)
 *   • O(1) removal from active list (doubly-linked)
 *   • No heap allocation per node after initial pool creation
 *   • Autoscaling = grow the array, relink the list
 *
 * The pool also maintains a FIFO task queue protected by its
 * own mutex + condition variable so worker threads sleep
 * cheaply while idle.
 * ========================================================= */

#include <pthread.h>
#include <stddef.h>

/* ── Forward declarations ─────────────────────────────────── */
typedef struct thread_node  thread_node_t;
typedef struct task_node    task_node_t;
typedef struct thread_pool  thread_pool_t;

/* ── Task (unit of work given to a worker thread) ─────────── */

/*
 * task_function_t
 *   A pointer to any function that takes a void* argument.
 *   Worker threads call this to do their actual work.
 */
typedef void (*task_function_t)(void *argument);

/*
 * task_node_t
 *   One item in the pool's FIFO task queue.
 *   The queue is a singly-linked list; 'next' points toward
 *   older tasks (tail direction).
 */
struct task_node {
    task_function_t  execute;   /* function to call                  */
    void            *argument;  /* argument forwarded to execute()   */
    task_node_t     *next;      /* next task in the FIFO queue       */
};

/* ── Thread node (intrusive linked list inside the array) ─── */

/*
 * thread_node_t
 *   Represents one worker thread.
 *   Lives at a fixed index inside thread_pool_t.worker_array[].
 *
 *   The prev/next pointers form an intrusive doubly-linked list
 *   of ACTIVE workers so we can remove any node in O(1) without
 *   scanning the whole array.
 */
struct thread_node {
    pthread_t       thread_id;      /* POSIX thread handle                     */
    int             slot_index;     /* index of this node in worker_array[]    */
    int             is_active;      /* 1 = running a task or waiting, 0 = free */
    thread_node_t  *prev;           /* previous node in active linked list     */
    thread_node_t  *next;           /* next     node in active linked list     */
    thread_pool_t  *pool;           /* back-pointer so worker can reach pool   */
};

/* ── The pool itself ──────────────────────────────────────── */

/*
 * thread_pool_t
 *   Central structure that owns workers, the task queue,
 *   and all synchronisation primitives.
 *
 *   worker_array   — heap-allocated array of thread_node_t.
 *                    Grown (realloced) during autoscale.
 *   active_head    — head of the active-worker linked list.
 *   capacity       — current allocated length of worker_array[].
 *   active_count   — number of nodes currently in the linked list.
 *   task_queue_*   — FIFO queue of pending tasks.
 *   shutdown_flag  — set to 1 to tell all workers to exit.
 */
struct thread_pool {
    /* Worker storage */
    thread_node_t  *worker_array;       /* flat array of thread nodes          */
    thread_node_t  *active_head;        /* head of the active doubly-linked list */
    int             capacity;           /* allocated slots in worker_array[]   */
    int             active_count;       /* workers currently in the linked list */

    /* Task queue (FIFO singly-linked list) */
    task_node_t    *task_queue_head;    /* oldest task (next to be consumed)   */
    task_node_t    *task_queue_tail;    /* newest task (most recently enqueued) */
    int             pending_task_count; /* number of tasks waiting             */

    /* Autoscale knobs */
    int             min_threads;        /* pool never shrinks below this       */
    int             max_threads;        /* pool never grows above this         */

    /* Synchronisation */
    pthread_mutex_t pool_mutex;         /* protects every field above          */
    pthread_cond_t  task_available_cond;/* workers wait here for new tasks     */
    pthread_cond_t  all_tasks_done_cond;/* thread_pool_wait() sleeps here      */

    /* Lifecycle */
    int             shutdown_flag;      /* 1 = workers should exit             */
};

/*
 * IDLE_TIMEOUT_SECONDS
 *   How long a worker waits on task_available_cond with no work
 *   before checking whether it should scale down.
 *
 *   Scale-down rule (enforced inside worker_thread_loop):
 *     If the timed-wait fires (no task arrived), the queue is still
 *     empty, AND active_count > min_threads → the worker unlinks
 *     itself from the active list and exits.
 *
 *   This guarantees:
 *     • Pool never drops below min_threads during quiet periods.
 *     • Pool never exceeds max_threads during busy periods.
 *     • Excess threads that were spawned during a burst are
 *       automatically reaped after IDLE_TIMEOUT_SECONDS of inactivity.
 */
#define IDLE_TIMEOUT_SECONDS  30

/* ── Public API ───────────────────────────────────────────── */

/*
 * thread_pool_create()
 *   Allocates and initialises the pool.
 *   Spawns `initial_threads` workers immediately.
 *   The pool will autoscale between min_threads and max_threads.
 *
 *   Returns: pointer to the new pool, or NULL on failure.
 */
thread_pool_t *thread_pool_create(int initial_threads,
                                  int min_threads,
                                  int max_threads);

/*
 * thread_pool_submit_task()
 *   Enqueues one task.  If all workers are busy and capacity
 *   allows, the pool spawns additional workers (autoscale up).
 *
 *   Returns: 0 on success, -1 on failure.
 */
int thread_pool_submit_task(thread_pool_t   *pool,
                            task_function_t  function,
                            void            *argument);

/*
 * thread_pool_wait_until_idle()
 *   Blocks the calling thread until the task queue is empty
 *   AND all workers have finished their current task.
 *   Useful for graceful shutdown.
 */
void thread_pool_wait_until_idle(thread_pool_t *pool);

/*
 * thread_pool_destroy()
 *   Signals all workers to exit, joins them, then frees all
 *   memory owned by the pool.
 */
void thread_pool_destroy(thread_pool_t *pool);

#endif /* THREAD_POOL_H */