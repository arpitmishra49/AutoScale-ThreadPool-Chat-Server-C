# CLI Chat Application in C
## Multithreaded Server with Autoscaling Thread Pool

---

## File Structure

```
chat_app/
├── Makefile                   ← builds both binaries
├── common/
│   └── protocol.h             ← shared wire-format struct & constants
├── server/
│   ├── thread_pool.h          ← thread pool API & data structure docs
│   ├── thread_pool.c          ← pool implementation (autoscale, linked list)
│   └── server.c               ← TCP server, client registry, broadcast
└── client/
    └── client.c               ← TCP client, send/receive threads
```

---

## Quick Start

```bash
# 1. Build everything
make

# 2. In terminal 1 — start the server
./server/server

# 3. In terminal 2, 3, 4 … — start clients
./client/client
```

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────┐
│                        SERVER                        │
│                                                      │
│  main thread                                         │
│    listen() → accept() loop                          │
│         │                                            │
│         ▼ submit_task(handle_client, fd)             │
│  ┌──────────────────────────────────┐                │
│  │       Thread Pool (autoscale)    │                │
│  │  [W0] [W1] [W2] ... [Wn]        │                │
│  │   ↑    ↑    ↑         ↑         │                │
│  │   intrusive doubly-linked list   │                │
│  │   inside flat array[]            │                │
│  └──────────────────────────────────┘                │
│         │                                            │
│         ▼  each worker runs handle_client_task()     │
│  ┌──────────────────────────────┐                    │
│  │  Client Registry (array)     │                    │
│  │  [C0]←→[C1]←→[C2]  (list)   │ ← registry_mutex  │
│  └──────────────────────────────┘                    │
│         │ broadcast_message_to_all_except_sender()   │
│         ▼                                            │
│  sends to every other client's socket_fd             │
└─────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────┐
│                        CLIENT                        │
│                                                      │
│   main thread: gets username, connects, sends login  │
│         │                                            │
│         ├── receive_thread: recv() → print           │
│         └── send_thread:    fgets() → send()         │
│                                                      │
│   Both share client_shared_state_t                   │
│   shutdown_flag protected by state_mutex             │
└─────────────────────────────────────────────────────┘
```

---

## Key Design Decisions

### 1. Hybrid Array + Intrusive Linked List (Thread Pool & Client Registry)

Both the thread pool and the client registry use the same pattern:

- **Flat array**: gives O(1) access by index, no fragmentation, cache-friendly.
- **Intrusive doubly-linked list** (thread pool) / singly-linked list (registry): enables O(1) insertion/removal without scanning the whole array.

Each node stores its own `slot_index` so even after a `realloc` (autoscale) we can rebuild the list pointers by index.

```
worker_array[]:   [0]  [1]  [2]  [3]  [4]  [5]
                   ●    ●         ●         ●     ← is_active
                   ↓    ↓         ↓         ↓
active_head → [0] ↔ [1] ↔ [3] ↔ [5] → NULL
```

### 2. Autoscaling Thread Pool

- Starts with `INITIAL_WORKER_THREADS`.
- On `submit_task()`: if all workers are busy and `active_count < max_threads`, a new worker is spawned.
- If the array is full, `worker_array` is `realloc`-ed to double capacity, the linked list is rebuilt from `slot_index` fields, then a new worker is spawned.
- Workers never shrink back (scale-down is omitted for clarity; easy to add).

### 3. Mutex-Protected Shared State

| Mutex | What it protects |
|-------|-----------------|
| `pool->pool_mutex` | task queue, active list, worker count, shutdown flag |
| `registry_mutex` (server) | client registry array + active clients linked list |
| `state->state_mutex` (client) | `shutdown_flag` shared between send & receive threads |

### 4. Fixed-Size Wire Protocol

`chat_message_t` has a fixed binary layout declared in `common/protocol.h`.  Both client and server `send()`/`recv()` exactly `sizeof(chat_message_t)` bytes per message.  This avoids the complexity of framing/parsing a variable-length protocol.

### 5. Two-Thread Client Model

```
┌──────────────┐          ┌───────────────┐
│ send_thread  │          │ receive_thread│
│ fgets(stdin) │          │ recv(sock)    │
│ → send(sock) │          │ → printf      │
└──────┬───────┘          └───────┬───────┘
       │   shared_state.shutdown_flag      │
       └──────────── mutex ────────────────┘
```

Either thread can set `shutdown_flag = 1`.  The other thread checks it at the top of its loop and exits cleanly.

---

## Line-by-Line Code Explanations

### common/protocol.h

| Construct | Explanation |
|-----------|-------------|
| `#ifndef PROTOCOL_H` | Include guard: prevents double-inclusion in the same translation unit |
| `SERVER_PORT 8080` | Macro so port only needs changing in one place |
| `SERVER_BACKLOG 128` | Passed to `listen()`; the kernel queues up to 128 pending `connect()` calls |
| `chat_message_t` | The exact bytes travelling over TCP; `username` + `text`, both fixed-size char arrays |

---

### server/thread_pool.h

| Construct | Explanation |
|-----------|-------------|
| `task_function_t` | `typedef void (*)(void*)` — a pointer to any void-returning function |
| `task_node_t.next` | Singly-linked FIFO queue node; head = oldest, tail = newest |
| `thread_node_t.slot_index` | The index of this node inside `worker_array[]`; survives realloc |
| `thread_node_t.prev/next` | Doubly-linked list pointers for O(1) removal from the active set |
| `thread_node_t.pool` | Back-pointer so the worker function can reach the pool without a global |
| `pool_mutex` | Single lock protecting all pool state |
| `task_available_cond` | Workers `pthread_cond_wait` here when idle |
| `all_tasks_done_cond` | `thread_pool_wait_until_idle()` sleeps here |

---

### server/thread_pool.c

| Function | What it does |
|----------|--------------|
| `linked_list_insert_at_head()` | O(1) prepend to doubly-linked list; fixes prev/next of neighbours |
| `linked_list_remove_node()` | O(1) unlink; uses prev/next to bypass the node |
| `dequeue_oldest_task()` | Removes head of FIFO; sets tail=NULL when last task is taken |
| `enqueue_new_task()` | Appends to FIFO tail; allocates a `task_node_t` on the heap |
| `initialise_worker_node()` | `memset` + fill metadata; does NOT start the thread |
| `worker_thread_loop()` | Each thread's `while(1)`: lock → wait on cond → dequeue → unlock → execute → repeat |
| `spawn_worker_into_slot()` | Finds free slot, inits node, inserts into list, calls `pthread_create` |
| `grow_worker_array_and_spawn()` | `realloc` doubles capacity; rebuilds the linked list from `slot_index`; spawns one worker |
| `thread_pool_create()` | Allocates pool, inits mutex/conds, calls `spawn_worker_into_slot` N times |
| `thread_pool_submit_task()` | Enqueues task; autoscales if needed; signals one worker with `pthread_cond_signal` |
| `thread_pool_destroy()` | Sets `shutdown_flag`; broadcasts to wake all workers; frees all memory |

---

### server/server.c

| Function | What it does |
|----------|--------------|
| `registry_find_free_slot()` | Linear scan for `is_connected == 0`; O(n) but n ≤ 64 |
| `registry_add_client()` | Fills a free slot, prepends to `active_clients_head` singly-linked list |
| `registry_remove_client()` | Walks the list with a `**indirect` pointer trick to splice out the node |
| `broadcast_message_to_all_except_sender()` | Locks registry_mutex, iterates the linked list, calls `send()` on each fd except sender's |
| `handle_client_connection_task()` | The task submitted to the pool per client: login → announce → receive loop → cleanup |
| `handle_shutdown_signal()` | `signal()` handler; closes `server_socket_fd` so `accept()` returns EBADF |
| `create_and_bind_listening_socket()` | `socket()` → `setsockopt(SO_REUSEADDR)` → `bind()` → `listen()` |
| `main()` | Installs signals, creates socket + pool, runs accept loop, graceful shutdown |

Key socket calls:
- `socket(AF_INET, SOCK_STREAM, 0)` — IPv4 TCP socket
- `htons(SERVER_PORT)` — host-to-network short: ensures big-endian port on the wire
- `htonl(INADDR_ANY)` — bind to all interfaces
- `accept()` — returns a NEW fd per connection; the listen fd stays open
- `inet_ntop()` — converts binary IPv4 address back to dotted-decimal string for logging

---

### client/client.c

| Function | What it does |
|----------|--------------|
| `shared_state_is_shutting_down()` | Locks mutex, reads flag, unlocks; avoids data race on `shutdown_flag` |
| `shared_state_request_shutdown()` | Locks mutex, sets flag = 1, unlocks |
| `receive_messages_from_server()` | Thread function; `recv()` loop; prints `[username]: text`; sets shutdown on disconnect |
| `send_messages_to_server()` | Thread function; `fgets()` loop; builds `chat_message_t`; partial-send loop; handles `/quit` |
| `connect_to_server()` | `socket()` → `inet_pton()` → `connect()` → returns connected fd |
| `main()` | Gets username → connects → sends login → inits shared state + mutex → spawns 2 threads → joins both → cleanup |

Key details:
- `inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr)` — string IP → binary
- The send loop retries until all `sizeof(chat_message_t)` bytes are delivered
- `pthread_join()` ensures we wait for both threads before `close(server_fd)`

---

## Running Multiple Clients

Open 3 terminals:

```bash
# Terminal 1
./server/server

# Terminal 2
./client/client
# Enter username: Alice
# > Hello everyone!

# Terminal 3
./client/client
# Enter username: Bob
# > Hi Alice!
```

Alice sees Bob's messages; Bob sees Alice's messages; the server logs everything.