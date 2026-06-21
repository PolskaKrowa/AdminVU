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
 *   GET  /api/guilds/<guild_id>/channels
 *   GET  /api/mod-logs          ?guild_id=X&action=all&limit=50&offset=0
 *   GET  /api/warnings          ?guild_id=X&user_id=Y
 *   GET  /api/propagation/guilds
 *   GET  /api/propagation/events ?limit=50&offset=0
 *   GET  /api/propagation/blocked
 *   POST /api/propagation/block   body: guild_id=X&reason=Y
 *   POST /api/propagation/unblock body: guild_id=X
 *   POST /api/send-message        body: guild_id=X&channel_id=Y&content=Z
 *   GET  /api/tickets           ?guild_id=X&status=all
 *   GET  /api/tickets/<id>
 *   GET  /api/tickets/<id>/log
 *   GET  /api/tickets/<id>/chat      live message history (JSON)
 *   GET  /api/tickets/<id>/archive   self-contained HTML transcript
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
 * Dispatch an API request and write the response body into *out_buf_ptr.
 *
 * method   – "GET" or "POST"
 * path     – URL path component, e.g. "/api/mod-logs"
 * query    – query string without leading '?', may be NULL
 * body     – POST body, may be NULL
 * body_len – length of body in bytes
 * out_buf  – caller-supplied initial output buffer (at least 65536 bytes
 *            recommended).  Used as-is for small responses; on large
 *            responses (e.g. multi-MB ticket archives) api_handle may
 *            malloc a bigger buffer and update *out_buf_ptr to point at
 *            it.  The caller MUST call api_handle_free_response() when
 *            done to release any heap allocation (it is a no-op if the
 *            response stayed in the caller's original buffer).
 * out_size – size of the initial out_buf
 *
 * Returns an HTTP status code: 200, 400, 404, or 500.
 *
 * On return, *out_buf_ptr points to the response body (NUL-terminated)
 * and *out_len_ptr holds its length in bytes.  The body is always a
 * valid C string — if the response was truncated for any reason, the
 * buffer contains whatever was successfully written before truncation
 * rather than being left empty.
 */
int api_handle(Database *db,
               const char *method, const char *path,
               const char *query,  const char *body, size_t body_len,
               char *out_buf, size_t out_size,
               char **out_buf_ptr, size_t *out_len_ptr);

/*
 * api_handle_free_response
 *
 * Releases any heap allocation made by api_handle() in the previous call.
 * Safe to call with a pointer that still points at the caller's original
 * (stack or static) buffer — the function tracks whether a heap allocation
 * was made and only frees it if so.
 *
 * Pass the SAME char** that you passed to api_handle() as out_buf_ptr.
 * After this call returns, *out_buf_ptr is reset to NULL so a double-free
 * is impossible.
 */
void api_handle_free_response(char **out_buf_ptr);

/*
 * api_set_response_content_type / api_get_response_content_type
 *
 * When api_handle() serves a non-JSON response (currently only
 * GET /api/tickets/<id>/archive which returns text/html), it calls
 * api_set_response_content_type() so the http_server can set the correct
 * Content-Type header.  The caller must read it immediately after
 * api_handle() returns and before any other call into this module.
 *
 * The default (when unset or after api_handle resets it) is
 * "application/json".
 */
void        api_set_response_content_type(const char *ct);
const char *api_get_response_content_type(void);

#endif /* API_H */