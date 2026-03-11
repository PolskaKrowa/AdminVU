#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

/*
 * http_server.h
 *
 * Minimal HTTP/1.0 static-file + JSON API server.
 * Serves static files from web_root and routes /api/* to the API layer.
 * Binds to 127.0.0.1 only.
 */

#include <stdint.h>
#include <pthread.h>
#include <stdbool.h>
#include "database.h"

#define HTTP_SERVER_DEFAULT_PORT 8080
#define HTTP_SERVER_MAX_PATH     512
#define HTTP_SERVER_BACKLOG      16

typedef struct {
    int       port;
    char      web_root[HTTP_SERVER_MAX_PATH];
    int       listen_fd;
    bool      running;
    pthread_t thread;
    Database *db;          /* used by the /api/* handler   */
} HttpServer;

/*
 * http_server_init  – bind socket, set root dir, store db reference.
 * Returns 0 on success, -1 on failure.
 */
int  http_server_init(HttpServer *srv, int port,
                      const char *web_root, Database *db);

/*
 * http_server_start – spawn the background thread.
 * Returns 0 on success, -1 on failure.
 */
int  http_server_start(HttpServer *srv);

/*
 * http_server_stop  – signal the server to stop and close the socket.
 */
void http_server_stop(HttpServer *srv);

#endif /* HTTP_SERVER_H */