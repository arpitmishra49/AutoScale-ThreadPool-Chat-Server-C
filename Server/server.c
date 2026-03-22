/* =========================================================
 * server.c — Chat server
 * =========================================================
 *
 * OVERVIEW
 * ─────────
 * 1. Creates a TCP/IPv4 listening socket bound to SERVER_PORT.
 * 2. Maintains a global client registry (array + linked list)
 *    protected by a mutex.
 * 3. Each accepted connection is handed to the thread pool as
 *    a task.  The worker thread handles that client for its
 *    entire lifetime.
 * 4. Every message received from a client is broadcast to ALL
 *    other connected clients.
 * 5. The thread pool autoscales between MIN and MAX workers.
 *
 * COMPILE (from chat_app/server/):
 *   gcc -Wall -Wextra -pthread server.c thread_pool.c -o server
 * ========================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

/* POSIX socket / networking headers */
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>    /* struct sockaddr_in, htons(), htonl() */
#include <arpa/inet.h>     /* inet_pton(), inet_ntop()             */

#include "../Common/protocol.h"
#include "thread_pool.h"

/* ── Server-side configuration ────────────────────────────── */
#define INITIAL_WORKER_THREADS  4   /* workers at startup              */
#define MIN_WORKER_THREADS      2   /* pool never shrinks below this   */
#define MAX_WORKER_THREADS      32  /* pool never grows above this     */
#define MAX_CONNECTED_CLIENTS   64  /* hard cap on simultaneous users  */

/* ── Per-client record ────────────────────────────────────── */

/*
 * client_record_t
 *   Everything the server needs to know about one connected client.
 *   Records are stored in client_registry[] (a flat array) and
 *   simultaneously form an intrusive singly-linked list of ACTIVE
 *   records — exactly the same hybrid approach used in the thread pool.
 *
 *   Fields:
 *     socket_fd    — the file descriptor returned by accept()
 *     username     — the name the client sent at login
 *     slot_index   — index in client_registry[] for O(1) removal
 *     is_connected — 0 means this slot is free
 *     next         — next active record in the linked list
 */
typedef struct client_record client_record_t;
struct client_record {
    int              socket_fd;
    char             username[MAX_USERNAME_LEN];
    int              slot_index;
    int              is_connected;
    client_record_t *next;          /* intrusive linked list pointer */
};

/* ── Global state ─────────────────────────────────────────── */

/*
 * client_registry[]
 *   Flat array of client_record_t.  Slots are reused as clients
 *   connect and disconnect.
 *
 * active_clients_head
 *   Head of the singly-linked list of active (is_connected == 1)
 *   records within client_registry[].
 *
 * registry_mutex
 *   Protects both client_registry[] and active_clients_head.
 *   Any thread that reads or writes these MUST hold this mutex.
 *
 * connected_client_count
 *   Number of currently connected clients (length of the linked list).
 *
 * server_socket_fd
 *   The global listening socket; used by the signal handler to close it.
 *
 * server_thread_pool
 *   The autoscaling pool that handles client connections.
 */
static client_record_t  client_registry[MAX_CONNECTED_CLIENTS];
static client_record_t *active_clients_head   = NULL;
static pthread_mutex_t  registry_mutex        = PTHREAD_MUTEX_INITIALIZER;
static int              connected_client_count = 0;
static int              server_socket_fd       = -1;
static thread_pool_t   *server_thread_pool     = NULL;

/* ── Registry helpers ─────────────────────────────────────── */

/*
 * registry_find_free_slot()
 *   Scans client_registry[] for a slot with is_connected == 0.
 *   Caller MUST hold registry_mutex.
 *   Returns the slot index, or -1 if the registry is full.
 */
static int registry_find_free_slot(void)
{
    for (int i = 0; i < MAX_CONNECTED_CLIENTS; i++) {
        if (!client_registry[i].is_connected) {
            return i;
        }
    }
    return -1; /* registry is full */
}

/*
 * registry_add_client()
 *   Fills a free slot in client_registry[] and prepends the record
 *   to the active_clients_head singly-linked list.
 *   Caller MUST hold registry_mutex.
 *   Returns a pointer to the new record, or NULL if the registry is full.
 */
static client_record_t *registry_add_client(int socket_fd,
                                             const char *username)
{
    int slot = registry_find_free_slot();
    if (slot == -1) {
        return NULL;
    }

    client_record_t *record = &client_registry[slot];

    record->socket_fd    = socket_fd;
    record->slot_index   = slot;
    record->is_connected = 1;
    strncpy(record->username, username, MAX_USERNAME_LEN - 1);
    record->username[MAX_USERNAME_LEN - 1] = '\0'; /* ensure null termination */

    /* Prepend to the active linked list */
    record->next         = active_clients_head;
    active_clients_head  = record;

    connected_client_count++;
    return record;
}

/*
 * registry_remove_client()
 *   Marks `record` as disconnected and unlinks it from the singly-
 *   linked list.  Caller MUST hold registry_mutex.
 *
 *   Singly-linked removal requires a linear scan to find the
 *   predecessor, so this is O(n).  For MAX_CONNECTED_CLIENTS=64
 *   this is perfectly acceptable.
 */
static void registry_remove_client(client_record_t *record)
{
    /* Walk the list to find the node just before `record` */
    client_record_t **indirect = &active_clients_head;

    while (*indirect != NULL && *indirect != record) {
        indirect = &(*indirect)->next;
    }

    if (*indirect == record) {
        /* Skip over `record` in the list */
        *indirect = record->next;
    }

    /* Clear the slot so it can be reused */
    record->is_connected = 0;
    record->socket_fd    = -1;
    record->next         = NULL;

    connected_client_count--;
}

/* ── Broadcasting ─────────────────────────────────────────── */

/*
 * broadcast_message_to_all_except_sender()
 *   Walks the active_clients_head linked list and calls send()
 *   on every client EXCEPT the one whose socket_fd matches
 *   `sender_socket_fd`.
 *
 *   The function acquires registry_mutex so that the list is
 *   stable while we iterate.  send() is called INSIDE the lock;
 *   this is safe because send() on a non-blocking-or-closed
 *   descriptor does not deadlock but could block briefly — for
 *   a chat app at this scale that is fine.
 *
 *   Parameters:
 *     sender_socket_fd — file descriptor of the sending client
 *                        (excluded from the broadcast)
 *     message          — pointer to the chat_message_t to send
 */
static void broadcast_message_to_all_except_sender(int sender_socket_fd,
                                                   const chat_message_t *message)
{
    pthread_mutex_lock(&registry_mutex);

    client_record_t *current = active_clients_head;

    while (current != NULL) {
        /* Skip the sender themselves */
        if (current->socket_fd != sender_socket_fd) {
            /*
             * send() may not deliver the entire struct in one call
             * if the kernel buffer is full, so we loop until all
             * bytes are sent or an error occurs.
             */
            size_t total_bytes  = sizeof(chat_message_t);
            size_t bytes_sent   = 0;
            const char *buf_ptr = (const char *)message;

            while (bytes_sent < total_bytes) {
                ssize_t result = send(current->socket_fd,
                                      buf_ptr + bytes_sent,
                                      total_bytes - bytes_sent,
                                      0 /* flags */);
                if (result <= 0) {
                    /* Client likely disconnected; stop trying */
                    break;
                }
                bytes_sent += (size_t)result;
            }
        }
        current = current->next; /* advance to the next active client */
    }

    pthread_mutex_unlock(&registry_mutex);
}

/* ── Per-client worker task ───────────────────────────────── */

/*
 * handle_client_connection_task()
 *   This is the task_function_t that gets submitted to the thread
 *   pool for every newly accepted connection.
 *
 *   Lifecycle for one client:
 *     1. Receive the login username (first message from client).
 *     2. Register the client in the registry.
 *     3. Announce the join to all other clients.
 *     4. Loop: receive messages, broadcast them.
 *     5. On disconnect / error: announce departure, unregister, close fd.
 *
 *   `arg` is a heap-allocated int* holding the connected socket fd.
 *   We free it at the start.
 */
static void handle_client_connection_task(void *arg)
{
    /* Unpack and immediately free the heap-allocated fd */
    int client_fd = *((int *)arg);
    free(arg);

    /* ── Step 1: Receive login username ─────────────────── */
    chat_message_t login_message;
    memset(&login_message, 0, sizeof(login_message));

    ssize_t bytes_received = recv(client_fd,
                                  &login_message,
                                  sizeof(login_message),
                                  0 /* flags */);

    if (bytes_received <= 0) {
        /* Client disconnected before sending username */
        fprintf(stderr, "[Server] Client fd=%d disconnected before login\n",
                client_fd);
        close(client_fd);
        return;
    }

    /* Sanitise: ensure username is null-terminated */
    login_message.username[MAX_USERNAME_LEN - 1] = '\0';

    printf("[Server] New client connected: '%s' (fd=%d)\n",
           login_message.username, client_fd);

    /* ── Step 2: Register the client ────────────────────── */
    pthread_mutex_lock(&registry_mutex);
    client_record_t *client_record = registry_add_client(client_fd,
                                                          login_message.username);
    pthread_mutex_unlock(&registry_mutex);

    if (client_record == NULL) {
        /* Registry full — reject the client politely */
        fprintf(stderr, "[Server] Registry full, rejecting '%s'\n",
                login_message.username);
        close(client_fd);
        return;
    }

    /* ── Step 3: Announce the join ───────────────────────── */
    chat_message_t join_announcement;
    memset(&join_announcement, 0, sizeof(join_announcement));
    strncpy(join_announcement.username, "SERVER", MAX_USERNAME_LEN - 1);
    snprintf(join_announcement.text, MAX_MESSAGE_LEN,
             "*** %s has joined the chat! (%d users online) ***",
             login_message.username, connected_client_count);

    broadcast_message_to_all_except_sender(client_fd, &join_announcement);

    /* ── Step 4: Main receive-broadcast loop ─────────────── */
    chat_message_t incoming_message;

    while (1) {
        memset(&incoming_message, 0, sizeof(incoming_message));

        bytes_received = recv(client_fd,
                              &incoming_message,
                              sizeof(incoming_message),
                              0 /* flags */);

        if (bytes_received <= 0) {
            /*
             * recv() returns 0 on clean close, negative on error.
             * In both cases we treat this as a disconnect.
             */
            break;
        }

        /* Sanitise text before broadcasting */
        incoming_message.username[MAX_USERNAME_LEN - 1] = '\0';
        incoming_message.text[MAX_MESSAGE_LEN - 1]      = '\0';

        printf("[Server] '%s' says: %s\n",
               incoming_message.username, incoming_message.text);

        /* Broadcast to everyone except the sender */
        broadcast_message_to_all_except_sender(client_fd, &incoming_message);
    }

    /* ── Step 5: Clean up on disconnect ──────────────────── */
    printf("[Server] Client '%s' (fd=%d) disconnected\n",
           client_record->username, client_fd);

    /* Announce the departure */
    chat_message_t leave_announcement;
    memset(&leave_announcement, 0, sizeof(leave_announcement));
    strncpy(leave_announcement.username, "SERVER", MAX_USERNAME_LEN - 1);
    snprintf(leave_announcement.text, MAX_MESSAGE_LEN,
             "*** %s has left the chat ***",
             client_record->username);

    /* Remove from registry first so the leave count is accurate */
    pthread_mutex_lock(&registry_mutex);
    registry_remove_client(client_record);
    pthread_mutex_unlock(&registry_mutex);

    broadcast_message_to_all_except_sender(client_fd, &leave_announcement);

    /* Close the socket — this frees the kernel file-descriptor slot */
    close(client_fd);
}

/* ── Signal handling ──────────────────────────────────────── */

/*
 * handle_shutdown_signal()
 *   Caught on SIGINT (Ctrl-C) or SIGTERM.
 *   Closes the listening socket so that the accept() loop in
 *   main() returns an error and we can clean up gracefully.
 */
static void handle_shutdown_signal(int signal_number)
{
    printf("\n[Server] Received signal %d — shutting down...\n",
           signal_number);

    if (server_socket_fd != -1) {
        close(server_socket_fd);
        server_socket_fd = -1;
    }
}

/* ── Socket setup ─────────────────────────────────────────── */

/*
 * create_and_bind_listening_socket()
 *   Creates a TCP/IPv4 socket, sets SO_REUSEADDR (so we can
 *   restart the server quickly without waiting for TIME_WAIT),
 *   binds it to SERVER_PORT on all interfaces, and calls listen().
 *
 *   Returns the file descriptor of the listening socket,
 *   or exits the process on any failure.
 */
static int create_and_bind_listening_socket(void)
{
    /* AF_INET  = IPv4
     * SOCK_STREAM = TCP (reliable, connection-oriented)
     * 0 = let the OS pick the protocol (TCP for SOCK_STREAM) */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    /*
     * SO_REUSEADDR allows us to bind to a port that is in TIME_WAIT
     * state from a previous server run.  Without this, restarting
     * the server immediately after a crash would fail with EADDRINUSE.
     */
    int reuse_option = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse_option, sizeof(reuse_option)) == -1) {
        perror("setsockopt SO_REUSEADDR");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    /*
     * sockaddr_in holds the IPv4 address and port.
     * sin_family  = AF_INET (IPv4)
     * sin_port    = htons() converts host byte order → network byte order
     *               (big-endian on the wire)
     * sin_addr    = INADDR_ANY means "bind on all network interfaces"
     */
    struct sockaddr_in server_address;
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family      = AF_INET;
    server_address.sin_port        = htons(SERVER_PORT);
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_fd,
             (struct sockaddr *)&server_address,
             sizeof(server_address)) == -1) {
        perror("bind");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    /*
     * listen() marks the socket as passive.
     * SERVER_BACKLOG = maximum number of pending connections that
     * the kernel will queue before our accept() drains them.
     */
    if (listen(listen_fd, SERVER_BACKLOG) == -1) {
        perror("listen");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    return listen_fd;
}

/* ── main ─────────────────────────────────────────────────── */

int main(void)
{
    /* Install signal handlers for graceful shutdown */
    signal(SIGINT,  handle_shutdown_signal);
    signal(SIGTERM, handle_shutdown_signal);

    /* Initialise the client registry to all-empty */
    memset(client_registry, 0, sizeof(client_registry));

    /* Create the listening socket */
    server_socket_fd = create_and_bind_listening_socket();
    printf("[Server] Listening on port %d (IPv4/TCP)\n", SERVER_PORT);

    /* Create the autoscaling thread pool */
    server_thread_pool = thread_pool_create(INITIAL_WORKER_THREADS,
                                            MIN_WORKER_THREADS,
                                            MAX_WORKER_THREADS);
    if (server_thread_pool == NULL) {
        fprintf(stderr, "[Server] Failed to create thread pool\n");
        close(server_socket_fd);
        return EXIT_FAILURE;
    }

    printf("[Server] Thread pool ready (%d-%d workers)\n",
           MIN_WORKER_THREADS, MAX_WORKER_THREADS);
    printf("[Server] Waiting for connections...\n\n");

    /* ── Accept loop ────────────────────────────────────── */
    while (1) {
        struct sockaddr_in client_address;
        socklen_t          client_address_len = sizeof(client_address);

        /*
         * accept() blocks until a client connects.
         * It returns a NEW file descriptor for the specific connection.
         * The original listen_fd stays open to accept more connections.
         */
        int connected_fd = accept(server_socket_fd,
                                  (struct sockaddr *)&client_address,
                                  &client_address_len);

        if (connected_fd == -1) {
            if (errno == EINVAL || errno == EBADF) {
                /* listen socket was closed by the signal handler */
                break;
            }
            perror("accept");
            continue; /* transient error — try again */
        }

        /* Print the client's IP address for logging */
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET,                      /* IPv4                     */
                  &client_address.sin_addr,     /* source: in_addr struct   */
                  client_ip,                    /* destination: char buffer */
                  sizeof(client_ip));           /* buffer size              */

        printf("[Server] Incoming connection from %s:%d (fd=%d)\n",
               client_ip, ntohs(client_address.sin_port), connected_fd);

        /*
         * Heap-allocate the fd so it can be passed safely to the
         * thread pool.  The worker task frees this allocation
         * at the start of handle_client_connection_task().
         */
        int *fd_for_task = malloc(sizeof(int));
        if (fd_for_task == NULL) {
            perror("malloc fd_for_task");
            close(connected_fd);
            continue;
        }
        *fd_for_task = connected_fd;

        /* Submit the task to the pool; a worker will handle it */
        if (thread_pool_submit_task(server_thread_pool,
                                    handle_client_connection_task,
                                    fd_for_task) != 0) {
            fprintf(stderr, "[Server] Failed to submit task for fd=%d\n",
                    connected_fd);
            free(fd_for_task);
            close(connected_fd);
        }
    }

    /* ── Graceful shutdown ───────────────────────────────── */
    printf("[Server] Shutting down gracefully...\n");

    /* Wait for in-flight tasks to complete */
    thread_pool_wait_until_idle(server_thread_pool);

    /* Destroy the pool (joins workers and frees memory) */
    thread_pool_destroy(server_thread_pool);

    /* Destroy the registry mutex */
    pthread_mutex_destroy(&registry_mutex);

    printf("[Server] Goodbye.\n");
    return EXIT_SUCCESS;
}