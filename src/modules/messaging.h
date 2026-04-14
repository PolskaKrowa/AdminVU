/*
 * messaging.h
 *
 * Public interface for the messaging module.
 *
 * bot_send_message() is the single entry-point called by api.c's
 * handle_send_message() to dispatch a plain-text message to a Discord
 * channel via the Orca REST client.
 *
 * Call messaging_module_init() once at bot startup (after the discord
 * client has been created but before any REST calls are made) so that
 * the module holds a valid client pointer.
 */

#ifndef MESSAGING_H
#define MESSAGING_H

#include <stdint.h>
#include <sqlite3.h>
#include <orca/discord.h>

/* ---------------------------------------------------------------------------
 * messaging_module_init
 *
 * Must be called once during bot startup with the fully-initialised
 * discord client.  The pointer is stored internally and used by every
 * subsequent bot_send_message() call.
 * --------------------------------------------------------------------------- */
void messaging_module_init(struct discord *client);

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

#endif /* MESSAGING_H */