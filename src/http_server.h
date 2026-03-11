#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

/*
 * http_server.h
 *
 * Minimal single-threaded HTTP/1.0 static-file server.
 * Serves files from a configurable root directory on localhost.
 * Designed to run in a dedicated pthread so it does not block the bot.
 *
 * Usage:
 *   HttpServer srv;
 *   if (http_server_init(&srv, 8080, "web") == 0)
 *       http_server_start(&srv);   // spawns background thread
 *   ...
 *   http_server_stop(&srv);
 */

#include <stdint.h>
#include <pthread.h>
#include <stdbool.h>

#define HTTP_SERVER_DEFAULT_PORT 8080
#define HTTP_SERVER_MAX_PATH     512
#define HTTP_SERVER_BACKLOG      16

typedef struct {
    int      port;
    char     web_root[HTTP_SERVER_MAX_PATH]; /* directory containing static files */
    int      listen_fd;                      /* listening socket fd               */
    bool     running;
    pthread_t thread;
} HttpServer;

/*
 * http_server_init  – prepare the server (bind socket, set root dir).
 * Returns 0 on success, -1 on failure.
 */
int  http_server_init(HttpServer *srv, int port, const char *web_root);

/*
 * http_server_start – spawn the background thread; non-blocking.
 * Returns 0 on success, -1 on failure.
 */
int  http_server_start(HttpServer *srv);

/*
 * http_server_stop  – signal the server to stop and join the thread.
 */
void http_server_stop(HttpServer *srv);

#endif /* HTTP_SERVER_H */