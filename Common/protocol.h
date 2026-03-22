#ifndef PROTOCOL_H
#define PROTOCOL_H

/* =========================================================
 * protocol.h — Shared constants used by both server & client
 * =========================================================
 * This header is the single source of truth for every
 * "magic number" in the project.  Both server.c and client.c
 * #include this file so that any value only needs to change
 * in one place.
 * ========================================================= */

/* Network ------------------------------------------------- */
#define SERVER_PORT        8080      /* TCP port the server listens on                  */
#define SERVER_BACKLOG     128       /* max pending connections queued by listen()       */
#define SERVER_IP          "127.0.0.1" /* loopback — change to 0.0.0.0 to bind all NICs */

/* Message framing ----------------------------------------- */
#define MAX_MESSAGE_LEN    1024      /* max bytes in a single chat message payload       */
#define MAX_USERNAME_LEN   64        /* max bytes for a username (including '\0')        */

/*
 * A chat_message is the exact byte layout sent over the wire.
 * Both sender and receiver cast their buffer to this struct,
 * so the layout MUST be identical on both ends — hence the
 * shared header.
 *
 * Field breakdown:
 *   username  — null-terminated display name entered at login
 *   text      — null-terminated message body typed by the user
 */
typedef struct {
    char username[MAX_USERNAME_LEN];
    char text[MAX_MESSAGE_LEN];
} chat_message_t;

#endif /* PROTOCOL_H */