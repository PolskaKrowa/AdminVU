/*
 * messaging.c
 *
 * Implements bot_send_message(), which is forward-declared in api.c and
 * called by handle_send_message() to dispatch a plain-text message to a
 * Discord channel via the Orca REST client.
 *
 * Threading note
 * --------------
 * Orca's discord_create_message() is documented as safe to call from any
 * thread that holds the client pointer.  The g_client pointer itself is
 * written once during bot startup (messaging_module_init) before any REST
 * calls are made, so no mutex is required for the pointer itself.
 */

#include "messaging.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <orca/discord.h>

/* ---------------------------------------------------------------------------
 * Module-private state
 * ---------------------------------------------------------------------------
 * Set once by messaging_module_init(); never written to again.
 * --------------------------------------------------------------------------- */
static struct discord *g_client = NULL;

/* ---------------------------------------------------------------------------
 * messaging_module_init
 * --------------------------------------------------------------------------- */
void messaging_module_init(struct discord *client) {
    g_client = client;
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