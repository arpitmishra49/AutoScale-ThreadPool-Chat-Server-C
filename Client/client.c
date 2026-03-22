/* =========================================================
 * client.c — CLI chat client
 * =========================================================
 *
 * OVERVIEW
 * ─────────
 * 1. Prompts the user for a username.
 * 2. Connects to the server over TCP/IPv4.
 * 3. Sends a login message (username only, no text).
 * 4. Spawns two POSIX threads:
 *      a. receive_thread — blocks on recv(), prints incoming messages.
 *      b. send_thread    — reads stdin line by line, sends messages.
 * 5. Either thread can set a shared shutdown_flag to tell the
 *    other to exit.  Both threads share a mutex to read/write it
 *    safely (inter-process communication via mutex + flag).
 *
 * COMPILE (from chat_app/client/):
 *   gcc -Wall -Wextra -pthread client.c -o client
 * ========================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

/* POSIX networking headers */
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>    /* struct sockaddr_in */
#include <arpa/inet.h>     /* inet_pton()        */

#include "../Common/protocol.h"

/* ── Shared state between the two client threads ──────────── */

/*
 * client_shared_state_t
 *   Both the send thread and the receive thread hold a pointer
 *   to this struct.  Every field is protected by `state_mutex`.
 *
 *   Fields:
 *     server_socket_fd — the connected socket to the server
 *     username         — the name this client logged in with
 *     shutdown_flag    — set to 1 by either thread to signal exit
 *     state_mutex      — guards shutdown_flag (and could guard more)
 */
typedef struct {
    int            server_socket_fd;
    char           username[MAX_USERNAME_LEN];
    int            shutdown_flag;
    pthread_mutex_t state_mutex;
} client_shared_state_t;

/* ── Helper: check shutdown flag safely ───────────────────── */

/*
 * shared_state_is_shutting_down()
 *   Acquires state_mutex, reads shutdown_flag, releases the mutex.
 *   Returns 1 if the flag is set, 0 otherwise.
 *
 *   Using a helper avoids forgetting to lock/unlock in the loops.
 */
static int shared_state_is_shutting_down(client_shared_state_t *state)
{
    pthread_mutex_lock(&state->state_mutex);
    int flag = state->shutdown_flag;
    pthread_mutex_unlock(&state->state_mutex);
    return flag;
}

/*
 * shared_state_request_shutdown()
 *   Sets shutdown_flag = 1 under the mutex so the other thread
 *   will notice on its next check.
 */
static void shared_state_request_shutdown(client_shared_state_t *state)
{
    pthread_mutex_lock(&state->state_mutex);
    state->shutdown_flag = 1;
    pthread_mutex_unlock(&state->state_mutex);
}

/* ── Receive thread ───────────────────────────────────────── */

/*
 * receive_messages_from_server()
 *   Entry point for the receive thread.
 *   Loops calling recv() on the server socket.
 *   On each complete message, prints "[username]: text" to stdout.
 *   On disconnect or error, sets the shutdown flag and exits.
 *
 *   `arg` is a pointer to the shared client_shared_state_t.
 */
static void *receive_messages_from_server(void *arg)
{
    client_shared_state_t *state = (client_shared_state_t *)arg;

    chat_message_t incoming_message;

    while (!shared_state_is_shutting_down(state)) {
        memset(&incoming_message, 0, sizeof(incoming_message));

        /*
         * recv() blocks until data arrives, the connection closes,
         * or an error occurs.
         * We receive exactly sizeof(chat_message_t) bytes because both
         * ends agreed on this fixed-size wire format in protocol.h.
         */
        ssize_t bytes_received = recv(state->server_socket_fd,
                                      &incoming_message,
                                      sizeof(incoming_message),
                                      0 /* flags */);

        if (bytes_received <= 0) {
            if (bytes_received == 0) {
                /* Server closed the connection cleanly */
                printf("\n[Client] Server closed the connection.\n");
            } else {
                /* recv() returned an error */
                if (!shared_state_is_shutting_down(state)) {
                    perror("\n[Client] recv error");
                }
            }
            /* Signal the send thread to exit too */
            shared_state_request_shutdown(state);
            break;
        }

        /* Defensive: ensure strings are null-terminated */
        incoming_message.username[MAX_USERNAME_LEN - 1] = '\0';
        incoming_message.text[MAX_MESSAGE_LEN - 1]      = '\0';

        /*
         * Print the message.  We use "\r\n" before the message to
         * overwrite any partially typed input line on the terminal,
         * then reprint the prompt so the UX looks clean.
         */
        printf("\r[%s]: %s\n> ", incoming_message.username,
                                  incoming_message.text);

        /* Flush stdout so the message appears immediately */
        fflush(stdout);
    }

    return NULL;
}

/* ── Send thread ──────────────────────────────────────────── */

/*
 * send_messages_to_server()
 *   Entry point for the send thread.
 *   Reads lines from stdin in a loop.
 *   Packages each line into a chat_message_t and sends it to the server.
 *   If the user types "/quit", initiates a graceful disconnect.
 *
 *   `arg` is a pointer to the shared client_shared_state_t.
 */
static void *send_messages_to_server(void *arg)
{
    client_shared_state_t *state = (client_shared_state_t *)arg;

    char input_buffer[MAX_MESSAGE_LEN];

    while (!shared_state_is_shutting_down(state)) {
        /* Print the prompt */
        printf("> ");
        fflush(stdout);

        /*
         * fgets() reads one line from stdin (up to MAX_MESSAGE_LEN-1 bytes).
         * It blocks until the user presses Enter.
         * Returns NULL on EOF (Ctrl-D) or error.
         */
        if (fgets(input_buffer, sizeof(input_buffer), stdin) == NULL) {
            /* EOF or read error — treat as quit */
            shared_state_request_shutdown(state);
            break;
        }

        /* Strip the trailing newline that fgets() includes */
        size_t input_len = strlen(input_buffer);
        if (input_len > 0 && input_buffer[input_len - 1] == '\n') {
            input_buffer[input_len - 1] = '\0';
            input_len--;
        }

        /* Ignore completely empty lines */
        if (input_len == 0) {
            continue;
        }

        /* Handle the /quit command */
        if (strcmp(input_buffer, "/quit") == 0) {
            printf("[Client] Disconnecting...\n");
            shared_state_request_shutdown(state);
            break;
        }

        /* Build the outgoing message struct */
        chat_message_t outgoing_message;
        memset(&outgoing_message, 0, sizeof(outgoing_message));

        /* Copy username from shared state */
        strncpy(outgoing_message.username,
                state->username,
                MAX_USERNAME_LEN - 1);

        /* Copy message text from the input buffer */
        strncpy(outgoing_message.text,
                input_buffer,
                MAX_MESSAGE_LEN - 1);

        /*
         * send() the full struct to the server.
         * Like recv(), send() may deliver fewer bytes than requested,
         * so we loop until all bytes are sent.
         */
        size_t total_bytes = sizeof(chat_message_t);
        size_t bytes_sent  = 0;
        const char *buf    = (const char *)&outgoing_message;

        while (bytes_sent < total_bytes) {
            ssize_t result = send(state->server_socket_fd,
                                  buf + bytes_sent,
                                  total_bytes - bytes_sent,
                                  0 /* flags */);
            if (result <= 0) {
                fprintf(stderr, "\n[Client] send error — server may be down\n");
                shared_state_request_shutdown(state);
                goto cleanup_send_thread; /* break out of both loops */
            }
            bytes_sent += (size_t)result;
        }
    }

cleanup_send_thread:
    return NULL;
}

/* ── Socket connection ────────────────────────────────────── */

/*
 * connect_to_server()
 *   Creates a TCP/IPv4 socket and connects it to SERVER_IP:SERVER_PORT.
 *
 *   Returns the connected socket file descriptor,
 *   or -1 on any failure.
 */
static int connect_to_server(void)
{
    /* Create a TCP socket (same flags as server side) */
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd == -1) {
        perror("socket");
        return -1;
    }

    /*
     * Fill in the server address.
     * inet_pton() converts the dotted-decimal string "127.0.0.1"
     * into the binary representation stored in sin_addr.
     * AF_INET = IPv4 family.
     */
    struct sockaddr_in server_address;
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port   = htons(SERVER_PORT);

    int convert_result = inet_pton(AF_INET,
                                   SERVER_IP,
                                   &server_address.sin_addr);
    if (convert_result != 1) {
        fprintf(stderr, "inet_pton: invalid address '%s'\n", SERVER_IP);
        close(sock_fd);
        return -1;
    }

    /* Initiate the TCP three-way handshake */
    if (connect(sock_fd,
                (struct sockaddr *)&server_address,
                sizeof(server_address)) == -1) {
        perror("connect");
        close(sock_fd);
        return -1;
    }

    return sock_fd;
}

/* ── main ─────────────────────────────────────────────────── */

int main(void)
{
    /* ── Step 1: Get username from the user ─────────────── */
    char username[MAX_USERNAME_LEN];

    printf("=== CLI Chat Client ===\n");
    printf("Enter your username: ");
    fflush(stdout);

    if (fgets(username, sizeof(username), stdin) == NULL) {
        fprintf(stderr, "Failed to read username\n");
        return EXIT_FAILURE;
    }

    /* Strip trailing newline */
    size_t uname_len = strlen(username);
    if (uname_len > 0 && username[uname_len - 1] == '\n') {
        username[uname_len - 1] = '\0';
    }

    if (strlen(username) == 0) {
        fprintf(stderr, "Username cannot be empty\n");
        return EXIT_FAILURE;
    }

    /* ── Step 2: Connect to the server ──────────────────── */
    printf("[Client] Connecting to %s:%d ...\n", SERVER_IP, SERVER_PORT);

    int server_fd = connect_to_server();
    if (server_fd == -1) {
        fprintf(stderr, "[Client] Could not connect to server\n");
        return EXIT_FAILURE;
    }

    printf("[Client] Connected! Type a message and press Enter.\n");
    printf("[Client] Type /quit to leave.\n\n");

    /* ── Step 3: Send login message ──────────────────────── */
    /*
     * The first message sent is just the username.
     * The server reads this to register the client and announce the join.
     * The text field is left empty for the login message.
     */
    chat_message_t login_message;
    memset(&login_message, 0, sizeof(login_message));
    strncpy(login_message.username, username, MAX_USERNAME_LEN - 1);
    /* login_message.text is intentionally left as all-zero '\0' bytes */

    if (send(server_fd, &login_message, sizeof(login_message), 0) == -1) {
        perror("[Client] Failed to send login message");
        close(server_fd);
        return EXIT_FAILURE;
    }

    /* ── Step 4: Initialise shared state ─────────────────── */
    client_shared_state_t shared_state;
    memset(&shared_state, 0, sizeof(shared_state));

    shared_state.server_socket_fd = server_fd;
    shared_state.shutdown_flag    = 0;
    strncpy(shared_state.username, username, MAX_USERNAME_LEN - 1);

    /*
     * Initialise the mutex that protects shutdown_flag.
     * Both threads will lock this mutex before reading or writing
     * the flag — this is the "safe inter-thread communication"
     * using mutexes referenced in the project spec.
     */
    pthread_mutex_init(&shared_state.state_mutex, NULL);

    /* ── Step 5: Spawn send & receive threads ───────────── */

    pthread_t receive_thread_id;
    pthread_t send_thread_id;

    /*
     * The receive thread runs receive_messages_from_server().
     * It blocks on recv() and prints incoming messages.
     */
    if (pthread_create(&receive_thread_id, NULL,
                       receive_messages_from_server, &shared_state) != 0) {
        perror("pthread_create receive_thread");
        close(server_fd);
        return EXIT_FAILURE;
    }

    /*
     * The send thread runs send_messages_to_server().
     * It blocks on fgets() and sends outgoing messages.
     */
    if (pthread_create(&send_thread_id, NULL,
                       send_messages_to_server, &shared_state) != 0) {
        perror("pthread_create send_thread");
        close(server_fd);
        return EXIT_FAILURE;
    }

    /* ── Step 6: Wait for both threads to finish ─────────── */
    /*
     * pthread_join() blocks until the named thread exits.
     * We wait for the send thread first (it exits on /quit or EOF),
     * then wait for the receive thread (it exits on server disconnect).
     */
    pthread_join(send_thread_id,     NULL);
    pthread_join(receive_thread_id,  NULL);

    /* ── Step 7: Clean up ────────────────────────────────── */
    pthread_mutex_destroy(&shared_state.state_mutex);
    close(server_fd);

    printf("[Client] Disconnected. Goodbye!\n");
    return EXIT_SUCCESS;
}