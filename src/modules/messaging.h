/*
 * messaging.h
 *
 * Public interface for the messaging module.
 *
 * bot_send_message()     – plain-text via Orca's discord_create_message()
 * bot_send_cv2_message() – Components V2 payload via libcurl (Discord v10 REST)
 *
 * Call messaging_module_init() once at bot startup (after the discord
 * client has been created) to register the Orca client pointer and the bot
 * token.  Both functions need the module to be initialised.
 */

#ifndef MESSAGING_H
#define MESSAGING_H

#include <stdint.h>
#include <sqlite3.h>
#include <orca/discord.h>
#include "database.h"
#include "components_v2.h"

/* ---------------------------------------------------------------------------
 * messaging_module_init
 *
 * Must be called once during bot startup with the fully-initialised discord
 * client and the bot token.  The token is forwarded to the components_v2
 * module so that cv2_send() can POST directly to Discord's REST API.
 * --------------------------------------------------------------------------- */
void messaging_module_init(struct discord *client, const char *bot_token);

/* ---------------------------------------------------------------------------
 * bot_send_message
 *
 * Send a plain-text message to the given Discord channel on behalf of
 * the bot.
 *
 * Parameters
 *   guild_id   – snowflake of the target guild   (used only for logging)
 *   channel_id – snowflake of the target channel
 *   content    – NUL-terminated UTF-8 text (max 2 000 chars per Discord)
 *
 * Returns
 *    0  on success
 *   -1  if the module has not been initialised (no client pointer)
 *   -2  if content is NULL or empty
 *   -3  if content exceeds Discord's 2 000-character limit
 *   -4  if the Orca REST call fails
 * --------------------------------------------------------------------------- */
int bot_send_message(sqlite3_int64 guild_id,
                     sqlite3_int64 channel_id,
                     const char   *content);

/* ---------------------------------------------------------------------------
 * bot_send_cv2_message
 *
 * Send a Components V2 message to a Discord channel using a pre-built CV2Msg.
 * Internally calls cv2_send(), which POSTs to the Discord REST API via
 * libcurl with the IS_COMPONENTS_V2 flag (1<<15) set.
 *
 * Parameters
 *   guild_id   – snowflake of the target guild (used only for logging)
 *   channel_id – snowflake of the target channel
 *   msg        – fully built CV2Msg (created with cv2_msg_new(), populated
 *                with cv2_add_text() / cv2_begin_container() etc., but NOT
 *                yet sent or freed)
 *
 * Returns
 *    0  on success (HTTP 2xx)
 *   -1  if the module has not been initialised
 *   -2  if msg is NULL or has a builder error
 *   -3  on curl / network failure
 *   HTTP status code (e.g. 403, 429) on a Discord API error
 *
 * The caller retains ownership of `msg` and must call cv2_msg_free() after
 * this function returns.
 * --------------------------------------------------------------------------- */
int bot_send_cv2_message(sqlite3_int64 guild_id,
                          sqlite3_int64 channel_id,
                          CV2Msg       *msg);

/* ---------------------------------------------------------------------------
 * messaging_refresh_guild_channels / messaging_refresh_guild_channels_async
 *
 * Fetch the guild's real channel list from Discord's REST API
 * (GET /guilds/{guild_id}/channels) and cache the sendable text channels
 * (type 0 = text, 5 = announcement) in the channels table.  Clears any
 * previously-cached channels for the guild first.
 *
 * The synchronous variant blocks for the duration of one HTTP GET (typically
 * < 1 s) and is safe to call from any thread.  The async variant spawns a
 * detached pthread so callers on the gateway / HTTP thread are not blocked.
 *
 * Returns (sync): the number of channels stored, or -1 on error.
 * Returns (async): void — failure is logged from the worker thread.
 *
 * Requires messaging_module_init() to have been called first (it stores the
 * bot token used for the REST Authorization header).
 * --------------------------------------------------------------------------- */
int  messaging_refresh_guild_channels(Database *db, u64_snowflake_t guild_id);
void messaging_refresh_guild_channels_async(Database *db, u64_snowflake_t guild_id);

#endif /* MESSAGING_H */