/*
 * messaging.c
 *
 * Implements bot_send_message() and bot_send_cv2_message().
 *
 * bot_send_message() uses Orca's discord_create_message() for plain text.
 *
 * bot_send_cv2_message() delegates to cv2_send() in components_v2.c, which
 * POSTs directly to Discord's REST API via libcurl -- the same approach used
 * in moderation.c for the timeout PATCH call -- because Orca's typed struct
 * layer predates the Components V2 API and does not expose the required
 * IS_COMPONENTS_V2 flag (1<<15).
 *
 * Threading note
 * --------------
 * Orca's discord_create_message() is safe to call from any thread that holds
 * the client pointer.  The g_client pointer is written once during bot startup
 * (messaging_module_init) before any REST calls are made, so no mutex is
 * required for the pointer itself.
 */

#include "messaging.h"
#include "components_v2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <orca/discord.h>

/* ---------------------------------------------------------------------------
 * Module-private state
 * ---------------------------------------------------------------------------
 * Both set once by messaging_module_init(); never written to again.
 * --------------------------------------------------------------------------- */
static struct discord *g_client = NULL;

/* ---------------------------------------------------------------------------
 * messaging_module_init
 * --------------------------------------------------------------------------- */
void messaging_module_init(struct discord *client, const char *bot_token) {
    g_client = client;
    /* Forward the token to the Components V2 module for REST calls. */
    cv2_components_init(bot_token);
    printf("[messaging] Module initialised.\n");
}

/* ---------------------------------------------------------------------------
 * bot_send_message
 * --------------------------------------------------------------------------- */
int bot_send_message(sqlite3_int64 guild_id,
                     sqlite3_int64 channel_id,
                     const char   *content) {

    /* ── Guard: module must be initialised ───────────────────────────────── */
    if (!g_client) {
        fprintf(stderr,
                "[messaging] ERROR: bot_send_message called before "
                "messaging_module_init().\n");
        return -1;
    }

    /* ── Guard: content must be non-empty ────────────────────────────────── */
    if (!content || content[0] == '\0') {
        fprintf(stderr,
                "[messaging] ERROR: bot_send_message called with NULL or "
                "empty content (guild=%" PRId64 " channel=%" PRId64 ").\n",
                guild_id, channel_id);
        return -2;
    }

    /* ── Guard: respect Discord's hard length limit ───────────────────────── */
    size_t content_len = strlen(content);
    if (content_len > DISCORD_MAX_MESSAGE_LEN) {
        fprintf(stderr,
                "[messaging] ERROR: content length %zu exceeds Discord "
                "limit of %d (guild=%" PRId64 " channel=%" PRId64 ").\n",
                content_len, DISCORD_MAX_MESSAGE_LEN, guild_id, channel_id);
        return -3;
    }

    printf("[messaging] Sending message to channel %" PRId64
           " (guild %" PRId64 ") – %zu char(s).\n",
           channel_id, guild_id, content_len);

    /* ── Build the create-message params struct ───────────────────────────── */
    struct discord_create_message_params params = {
        .content = (char *)content,   /* Orca treats this as read-only */
    };

    /* ── Dispatch via Orca ───────────────────────────────────────────────── */
    ORCAcode code = discord_create_message(g_client,
                                           (u64_snowflake_t)channel_id,
                                           &params,
                                           NULL /* ret – we don't need it */);

    if (code != ORCA_OK) {
        fprintf(stderr,
                "[messaging] ERROR: discord_create_message failed "
                "(ORCAcode %d) for channel %" PRId64
                " guild %" PRId64 ".\n",
                code, channel_id, guild_id);
        return -4;
    }

    printf("[messaging] Message delivered successfully to channel "
           "%" PRId64 ".\n", channel_id);
    return 0;
}

/* ---------------------------------------------------------------------------
 * bot_send_cv2_message
 * --------------------------------------------------------------------------- */
int bot_send_cv2_message(sqlite3_int64 guild_id,
                          sqlite3_int64 channel_id,
                          CV2Msg       *msg) {

    /* ── Guard: module must be initialised ───────────────────────────────── */
    if (!g_client) {
        fprintf(stderr,
                "[messaging] ERROR: bot_send_cv2_message called before "
                "messaging_module_init().\n");
        return -1;
    }

    /* ── Guard: builder must be valid ────────────────────────────────────── */
    if (!msg) {
        fprintf(stderr,
                "[messaging] ERROR: bot_send_cv2_message called with NULL msg "
                "(guild=%" PRId64 " channel=%" PRId64 ").\n",
                guild_id, channel_id);
        return -2;
    }

    printf("[messaging] Sending Components V2 message to channel %" PRId64
           " (guild %" PRId64 ").\n", channel_id, guild_id);

    /* ── Delegate to the cv2 module's REST sender ─────────────────────────
     * cv2_send() builds the JSON internally, POSTs it via libcurl, and
     * returns 0 on success or a negative/HTTP error code on failure.
     * --------------------------------------------------------------------- */
    int rc = cv2_send(msg, (uint64_t)channel_id);

    if (rc != 0) {
        fprintf(stderr,
                "[messaging] ERROR: cv2_send() failed (rc=%d) for channel "
                "%" PRId64 " guild %" PRId64 ".\n",
                rc, channel_id, guild_id);
        return -3;
    }

    printf("[messaging] Components V2 message delivered to channel "
           "%" PRId64 ".\n", channel_id);
    return 0;
}

/* ---------------------------------------------------------------------------
 * bot_send_cv2_raw
 *
 * Low-level shim used by api.c's handle_send_components_v2() when the caller
 * has already built the complete JSON payload string (envelope included).
 * This avoids constructing a CV2Msg just to send pre-built JSON from the
 * dashboard frontend.
 *
 * json_payload must be a valid, complete Discord message JSON string with
 * "flags":32768 already set, e.g.:
 *   {"flags":32768,"components":[{"type":10,"content":"Hello"}]}
 *
 * Returns 0 on success, -1 if module uninitialised, or the cv2 error code.
 * --------------------------------------------------------------------------- */
int bot_send_cv2_raw(sqlite3_int64 guild_id,
                      sqlite3_int64 channel_id,
                      const char   *json_payload) {
    if (!g_client) {
        fprintf(stderr,
                "[messaging] ERROR: bot_send_cv2_raw called before "
                "messaging_module_init().\n");
        return -1;
    }

    printf("[messaging] Sending raw Components V2 payload to channel "
           "%" PRId64 " (guild %" PRId64 ").\n", channel_id, guild_id);

    /*
     * cv2_post_raw() is a small internal helper exposed from components_v2.c
     * for this specific use case: posting a fully-formed payload string.
     */
    extern int cv2_post_raw(uint64_t channel_id, const char *json_payload);
    return cv2_post_raw((uint64_t)channel_id, json_payload);
}