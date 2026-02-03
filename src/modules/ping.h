#ifndef PING_H
#define PING_H

#include <orca/discord.h>

// Register the /ping slash command for the given guild.
// Must be called after the client is initialised and the bot is ready.
void register_slash_commands(struct discord *client,
                             u64_snowflake_t application_id,
                             u64_snowflake_t guild_id);

// Gateway event handler — dispatched for every interaction.
// Internally routes APPLICATION_COMMAND interactions to the
// appropriate handler (currently just /ping).
void on_interaction_create(struct discord *client,
                           const struct discord_interaction *event);

// Initialise the ping module: binds the interaction-create callback.
// guild_id is stored so that register_slash_commands() can be called
// from on_ready once the application_id is available.
void ping_module_init(struct discord *client, u64_snowflake_t guild_id);

#endif // PING_H