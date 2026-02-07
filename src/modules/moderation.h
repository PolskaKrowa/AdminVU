#ifndef MODERATION_H
#define MODERATION_H

#include <orca/discord.h>
#include "database.h"

// Initialise the moderation module
void moderation_module_init(struct discord *client, Database *db, u64_snowflake_t guild_id);

// Register moderation slash commands
void register_moderation_commands(struct discord *client,
                                   u64_snowflake_t application_id,
                                   u64_snowflake_t guild_id);

// Interaction handler for moderation commands
void on_moderation_interaction(struct discord *client,
                               const struct discord_interaction *event);

#endif // MODERATION_H