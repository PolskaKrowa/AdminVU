#ifndef API_H
#define API_H

/*
 * api.h
 *
 * Lightweight JSON REST API layer for the dashboard.
 *
 * Routes:
 *   GET  /api/status
 *   GET  /api/guilds
 *   GET  /api/mod-logs          ?guild_id=X&action=all&limit=50&offset=0
 *   GET  /api/warnings          ?guild_id=X&user_id=Y
 *   GET  /api/propagation/guilds
 *   GET  /api/propagation/events ?limit=50&offset=0
 *   GET  /api/propagation/blocked
 *   POST /api/propagation/block   body: guild_id=X&reason=Y
 *   POST /api/propagation/unblock body: guild_id=X
 *   GET  /api/tickets           ?guild_id=X&status=all
 *   GET  /api/tickets/<id>
 */

#include "database.h"
#include <stddef.h>

/*
 * api_init
 *
 * Creates any tables that the dashboard needs beyond the bot's own schema
 * (e.g. propagation_blocked_guilds).
 * Safe to call multiple times – uses CREATE TABLE IF NOT EXISTS.
 *
 * Returns 0 on success, -1 on failure.
 */
int api_init(Database *db);

/*
 * api_handle
 *
 * Dispatch an API request and write a JSON body into out_buf.
 *
 * method   – "GET" or "POST"
 * path     – URL path component, e.g. "/api/mod-logs"
 * query    – query string without leading '?', may be NULL
 * body     – POST body, may be NULL
 * body_len – length of body in bytes
 * out_buf  – caller-supplied output buffer (at least 65536 bytes recommended)
 * out_size – size of out_buf
 *
 * Returns an HTTP status code: 200, 400, 404, or 500.
 */
int api_handle(Database *db,
               const char *method, const char *path,
               const char *query,  const char *body, size_t body_len,
               char *out_buf, size_t out_size);

#endif /* API_H */