#ifndef PING_H
#define PING_H

#include <orca/discord.h>

// Register the /ping slash command for the given guild.
// Must be called after the client is initialised and the bot is ready.
void register_slash_commands(struct discord *client,
                             u64_snowflake_t application_id,
                             u64_snowflake_t guild_id);

// Gateway event handler for ping command interactions
void on_ping_interaction(struct discord *client,
                         const struct discord_interaction *event);

// Initialise the ping module: stores the guild_id for later use.
// The slash-command registration happens in on_ready once the
// application_id is available.
void ping_module_init(struct discord *client, u64_snowflake_t guild_id);

#endif // PING_H