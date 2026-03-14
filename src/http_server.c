/*
 * http_server.c
 *
 * Minimal HTTP/1.0 server.
 * - GET  requests to /api/* are dispatched to api_handle().
 * - POST requests to /api/* are dispatched with the request body.
 * - All other requests are served as static files from web_root.
 */

#include "http_server.h"
#include "api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <pthread.h>
#include <time.h>

/* ── Event ring buffer ──────────────────────────────────────────────────── */
#define SSE_MAX_EVENTS 128
#define SSE_MAX_JSON   512

typedef struct { char json[SSE_MAX_JSON]; long long ts; } SseEvent;
static SseEvent          g_sse_buf[SSE_MAX_EVENTS];
static int               g_sse_head  = 0;
static int               g_sse_count = 0;
static pthread_mutex_t   g_sse_mu    = PTHREAD_MUTEX_INITIALIZER;

void http_sse_push(const char *json) {
    if (!json) return;
    pthread_mutex_lock(&g_sse_mu);
    SseEvent *e = &g_sse_buf[g_sse_head % SSE_MAX_EVENTS];
    strncpy(e->json, json, SSE_MAX_JSON - 1);
    e->json[SSE_MAX_JSON - 1] = '\0';
    e->ts = (long long)time(NULL);
    g_sse_head++;
    if (g_sse_count < SSE_MAX_EVENTS) g_sse_count++;
    pthread_mutex_unlock(&g_sse_mu);
}

int http_sse_poll(long long since, char *out, size_t out_sz) {
    pthread_mutex_lock(&g_sse_mu);
    int n = 0;
    size_t p = 0;
    p += snprintf(out + p, out_sz - p, "[");
    int start = (g_sse_head > g_sse_count) ? g_sse_head - g_sse_count : 0;
    for (int i = start; i < g_sse_head && p + 32 < out_sz; i++) {
        SseEvent *e = &g_sse_buf[i % SSE_MAX_EVENTS];
        if (e->ts <= since) continue;
        if (n++ > 0) p += snprintf(out + p, out_sz - p, ",");
        p += snprintf(out + p, out_sz - p,
                      "{\"ts\":%lld,\"data\":%s}", e->ts, e->json);
    }
    snprintf(out + p, out_sz - p, "]");
    pthread_mutex_unlock(&g_sse_mu);
    return n;
}

/* ── MIME types ─────────────────────────────────────────────────────────── */

static const struct { const char *ext; const char *mime; } MIME_TABLE[] = {
    { ".html", "text/html; charset=utf-8"  },
    { ".css",  "text/css; charset=utf-8"   },
    { ".js",   "application/javascript"    },
    { ".json", "application/json"          },
    { ".png",  "image/png"                 },
    { ".jpg",  "image/jpeg"                },
    { ".jpeg", "image/jpeg"                },
    { ".gif",  "image/gif"                 },
    { ".svg",  "image/svg+xml"             },
    { ".ico",  "image/x-icon"              },
    { ".txt",  "text/plain; charset=utf-8" },
    { NULL, NULL }
};

static const char *mime_for_path(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    for (int i = 0; MIME_TABLE[i].ext; i++)
        if (strcasecmp(dot, MIME_TABLE[i].ext) == 0)
            return MIME_TABLE[i].mime;
    return "application/octet-stream";
}

/* ── Low-level write ────────────────────────────────────────────────────── */

static void send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n <= 0) break;
        sent += (size_t)n;
    }
}

static void send_str(int fd, const char *s) {
    send_all(fd, s, strlen(s));
}

/* ── Response builders ──────────────────────────────────────────────────── */

static void send_response(int fd, int status, const char *status_text,
                           const char *mime, const char *body, size_t body_len) {
    char header[512];
    snprintf(header, sizeof(header),
             "HTTP/1.0 %d %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "Access-Control-Allow-Origin: *\r\n"
             "Connection: close\r\n"
             "Cache-Control: no-cache\r\n"
             "\r\n",
             status, status_text, mime, body_len);
    send_str(fd, header);
    if (body && body_len > 0)
        send_all(fd, body, body_len);
}

static void send_error(int fd, int status, const char *text) {
    char body[256];
    snprintf(body, sizeof(body),
             "<html><body><h1>%d %s</h1></body></html>", status, text);
    send_response(fd, status, text, "text/html", body, strlen(body));
}

/* ── Static file serving ────────────────────────────────────────────────── */

static void serve_file(int fd, const char *filepath) {
    if (strstr(filepath, "..")) {
        send_error(fd, 403, "Forbidden");
        return;
    }
    FILE *f = fopen(filepath, "rb");
    if (!f) { send_error(fd, 404, "Not Found"); return; }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0 || size > 10 * 1024 * 1024) {
        fclose(f);
        send_error(fd, 500, "Internal Server Error");
        return;
    }

    char *buf = malloc((size_t)size);
    if (!buf) { fclose(f); send_error(fd, 500, "Internal Server Error"); return; }

    size_t nread = fread(buf, 1, (size_t)size, f);
    fclose(f);
    send_response(fd, 200, "OK", mime_for_path(filepath), buf, nread);
    free(buf);
}

/* ── Request handling ───────────────────────────────────────────────────── */

static void handle_client(int client_fd, HttpServer *srv) {
    /* Read the full request into a buffer. */
    char req[8192] = { 0 };
    size_t total = 0;
    while (total < sizeof(req) - 1) {
        ssize_t n = recv(client_fd, req + total, sizeof(req) - 1 - total, 0);
        if (n <= 0) break;
        total += (size_t)n;
        /* Stop once we have the header separator */
        if (memmem(req, total, "\r\n\r\n", 4)) break;
    }
    req[total] = '\0';
    if (total == 0) return;

    /* Parse request line: METHOD URL HTTP/x.x */
    char method[16] = { 0 };
    char raw_url[HTTP_SERVER_MAX_PATH] = { 0 };
    if (sscanf(req, "%15s %511s", method, raw_url) != 2) {
        send_error(client_fd, 400, "Bad Request");
        return;
    }

    /* Split path and query string */
    char url_path[HTTP_SERVER_MAX_PATH] = { 0 };
    char url_query[512] = { 0 };
    {
        char *q = strchr(raw_url, '?');
        if (q) {
            size_t plen = (size_t)(q - raw_url);
            if (plen >= sizeof(url_path)) plen = sizeof(url_path) - 1;
            memcpy(url_path, raw_url, plen);
            url_path[plen] = '\0';
            strncpy(url_query, q + 1, sizeof(url_query) - 1);
        } else {
            strncpy(url_path, raw_url, sizeof(url_path) - 1);
        }
    }

    /* Extract POST body (after \r\n\r\n header separator) */
    const char *body     = NULL;
    size_t      body_len = 0;
    {
        char *sep = memmem(req, total, "\r\n\r\n", 4);
        if (sep) {
            body     = sep + 4;
            body_len = total - (size_t)(body - req);
        }
    }

    /* ── Route: /api/* ────────────────────────────────────────────────── */
    if (strncmp(url_path, "/api/", 5) == 0 || strcmp(url_path, "/api") == 0) {
        if (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0) {
            send_error(client_fd, 405, "Method Not Allowed");
            return;
        }

        static char api_buf[131072]; /* 128 KB shared buffer (single-threaded server) */
        api_buf[0] = '\0';

        int status = api_handle(srv->db, method, url_path,
                                url_query[0] ? url_query : NULL,
                                body, body_len,
                                api_buf, sizeof(api_buf));

        const char *mime = "application/json";

        const char *status_text = "OK";
        if      (status == 299) { status = 200; mime = "text/html; charset=utf-8"; }
        else if (status == 400) status_text = "Bad Request";
        else if (status == 404) status_text = "Not Found";
        else if (status == 500) status_text = "Internal Server Error";

        send_response(client_fd, status, status_text,
                      "application/json",
                      api_buf, strlen(api_buf));
        return;
    }

    /* ── Route: static files ──────────────────────────────────────────── */
    if (strcmp(method, "GET") != 0) {
        send_error(client_fd, 405, "Method Not Allowed");
        return;
    }

    if (strcmp(url_path, "/") == 0)
        strncpy(url_path, "/index.html", sizeof(url_path) - 1);

    char filepath[HTTP_SERVER_MAX_PATH * 2];
    snprintf(filepath, sizeof(filepath), "%s%s", srv->web_root, url_path);
    serve_file(client_fd, filepath);
}

/* ── Server thread ──────────────────────────────────────────────────────── */

static void *server_thread(void *arg) {
    HttpServer *srv = (HttpServer *)arg;

    printf("[http] Listening on http://127.0.0.1:%d/  (root: %s)\n",
           srv->port, srv->web_root);

    while (srv->running) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        int client_fd = accept(srv->listen_fd,
                               (struct sockaddr *)&client_addr, &addrlen);
        if (client_fd < 0) {
            if (srv->running) perror("[http] accept");
            break;
        }

        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        handle_client(client_fd, srv);
        close(client_fd);
    }

    printf("[http] Server thread exiting.\n");
    return NULL;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

int http_server_init(HttpServer *srv, int port,
                     const char *web_root, Database *db) {
    memset(srv, 0, sizeof *srv);
    srv->port = port;
    srv->db   = db;
    strncpy(srv->web_root, web_root, sizeof(srv->web_root) - 1);

    /* Initialize the API (creates any extra tables) */
    if (db && api_init(db) != 0)
        fprintf(stderr, "[http] api_init warning: some tables may be missing.\n");

    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->listen_fd < 0) { perror("[http] socket"); return -1; }

    int opt = 1;
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };

    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[http] bind"); close(srv->listen_fd); return -1;
    }
    if (listen(srv->listen_fd, HTTP_SERVER_BACKLOG) < 0) {
        perror("[http] listen"); close(srv->listen_fd); return -1;
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
    if (srv->listen_fd >= 0) {
        close(srv->listen_fd);
        srv->listen_fd = -1;
    }
}