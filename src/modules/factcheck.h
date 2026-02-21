#ifndef FACTCHECK_H
#define FACTCHECK_H

#include <orca/discord.h>

/**
 * @brief Initialise the factcheck module.
 *
 * Call once from main() after discord_init(). Stores the bot's own user ID
 * so the message handler can detect when the bot is @-mentioned.
 *
 * @param client  The Discord client.
 */
void factcheck_module_init(struct discord *client);

/**
 * @brief Message handler – wire this up via discord_set_on_message_create().
 *
 * Triggers when a user replies to any message and mentions the bot with
 * "is this true?" in the same message.  The replied-to message content is
 * forwarded to a local Ollama instance, which is prompted to produce a
 * convincing (and completely fabricated) argument for why the statement is
 * false.  The result is sent back as a Discord reply to the original user.
 *
 * @param client  The Discord client.
 * @param msg     The incoming message event.
 */
void on_factcheck_message(struct discord *client,
                          const struct discord_message *msg);

#endif /* FACTCHECK_H */