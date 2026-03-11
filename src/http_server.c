/*
 * http_server.c
 *
 * Minimal HTTP/1.0 static-file server.
 *
 * Only GET requests are handled.  Everything else receives 405.
 * Path traversal ("../") is blocked; the server will return 403.
 * Unknown extensions get "application/octet-stream".
 */

#include "http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ── MIME types ─────────────────────────────────────────────────────────── */

static const struct { const char *ext; const char *mime; } MIME_TABLE[] = {
    { ".html", "text/html; charset=utf-8"       },
    { ".css",  "text/css; charset=utf-8"         },
    { ".js",   "application/javascript"          },
    { ".json", "application/json"                },
    { ".png",  "image/png"                       },
    { ".jpg",  "image/jpeg"                      },
    { ".jpeg", "image/jpeg"                      },
    { ".gif",  "image/gif"                       },
    { ".svg",  "image/svg+xml"                   },
    { ".ico",  "image/x-icon"                    },
    { ".txt",  "text/plain; charset=utf-8"       },
    { NULL, NULL }
};

static const char *mime_for_path(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    for (int i = 0; MIME_TABLE[i].ext; i++) {
        if (strcasecmp(dot, MIME_TABLE[i].ext) == 0)
            return MIME_TABLE[i].mime;
    }
    return "application/octet-stream";
}

/* ── Response helpers ───────────────────────────────────────────────────── */

static void send_str(int fd, const char *s) {
    size_t len = strlen(s);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, s + sent, len - sent);
        if (n <= 0) break;
        sent += (size_t)n;
    }
}

static void send_response(int fd, int status, const char *status_text,
                           const char *mime, const char *body, size_t body_len) {
    char header[512];
    snprintf(header, sizeof(header),
             "HTTP/1.0 %d %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "Cache-Control: no-cache\r\n"
             "\r\n",
             status, status_text, mime, body_len);
    send_str(fd, header);

    if (body && body_len > 0) {
        size_t sent = 0;
        while (sent < body_len) {
            ssize_t n = write(fd, body + sent, body_len - sent);
            if (n <= 0) break;
            sent += (size_t)n;
        }
    }
}

static void send_error(int fd, int status, const char *text) {
    char body[256];
    snprintf(body, sizeof(body),
             "<html><body><h1>%d %s</h1></body></html>", status, text);
    send_response(fd, status, text, "text/html", body, strlen(body));
}

/* ── File serving ───────────────────────────────────────────────────────── */

static void serve_file(int fd, const char *filepath) {
    /* Block path traversal */
    if (strstr(filepath, "..")) {
        send_error(fd, 403, "Forbidden");
        return;
    }

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        send_error(fd, 404, "Not Found");
        return;
    }

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0 || size > 10 * 1024 * 1024 /* 10 MB cap */) {
        fclose(f);
        send_error(fd, 500, "Internal Server Error");
        return;
    }

    char *buf = malloc((size_t)size);
    if (!buf) {
        fclose(f);
        send_error(fd, 500, "Internal Server Error");
        return;
    }

    size_t nread = fread(buf, 1, (size_t)size, f);
    fclose(f);

    const char *mime = mime_for_path(filepath);
    send_response(fd, 200, "OK", mime, buf, nread);
    free(buf);
}

/* ── Request handling ───────────────────────────────────────────────────── */

static void handle_client(int client_fd, const char *web_root) {
    /* Read the request line (we only need the first line). */
    char req[2048] = { 0 };
    ssize_t n = recv(client_fd, req, sizeof(req) - 1, 0);
    if (n <= 0) return;
    req[n] = '\0';

    /* Parse method and URL path from the first line. */
    char method[16] = { 0 };
    char url[HTTP_SERVER_MAX_PATH] = { 0 };
    if (sscanf(req, "%15s %511s", method, url) != 2) {
        send_error(client_fd, 400, "Bad Request");
        return;
    }

    if (strcmp(method, "GET") != 0) {
        send_error(client_fd, 405, "Method Not Allowed");
        return;
    }

    /* Strip query string */
    char *q = strchr(url, '?');
    if (q) *q = '\0';

    /* Default to index.html */
    if (strcmp(url, "/") == 0)
        strncpy(url, "/index.html", sizeof(url) - 1);

    /* Build filesystem path: web_root + url */
    char filepath[HTTP_SERVER_MAX_PATH * 2];
    snprintf(filepath, sizeof(filepath), "%s%s", web_root, url);

    serve_file(client_fd, filepath);
}

/* ── Server thread ──────────────────────────────────────────────────────── */

static void *server_thread(void *arg) {
    HttpServer *srv = (HttpServer *)arg;

    printf("[http] Server listening on http://127.0.0.1:%d/ (root: %s)\n",
           srv->port, srv->web_root);

    while (srv->running) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);

        int client_fd = accept(srv->listen_fd,
                               (struct sockaddr *)&client_addr, &addrlen);
        if (client_fd < 0) {
            if (srv->running)
                perror("[http] accept");
            break;
        }

        /* Set a short receive timeout so a slow client can't stall the loop. */
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        handle_client(client_fd, srv->web_root);
        close(client_fd);
    }

    printf("[http] Server thread exiting.\n");
    return NULL;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

int http_server_init(HttpServer *srv, int port, const char *web_root) {
    memset(srv, 0, sizeof *srv);
    srv->port = port;
    strncpy(srv->web_root, web_root, sizeof(srv->web_root) - 1);

    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->listen_fd < 0) {
        perror("[http] socket");
        return -1;
    }

    /* Allow immediate reuse of the port after restart. */
    int opt = 1;
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK), /* 127.0.0.1 only */
    };

    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[http] bind");
        close(srv->listen_fd);
        return -1;
    }

    if (listen(srv->listen_fd, HTTP_SERVER_BACKLOG) < 0) {
        perror("[http] listen");
        close(srv->listen_fd);
        return -1;
    }

    return 0;
}

int http_server_start(HttpServer *srv) {
    srv->running = true;
    if (pthread_create(&srv->thread, NULL, server_thread, srv) != 0) {
        perror("[http] pthread_create");
        srv->running = false;
        return -1;
    }
    pthread_detach(srv->thread);
    return 0;
}

void http_server_stop(HttpServer *srv) {
    if (!srv->running) return;
    srv->running = false;
    /* Closing the listening socket unblocks accept(). */
    if (srv->listen_fd >= 0) {
        close(srv->listen_fd);
        srv->listen_fd = -1;
    }
}