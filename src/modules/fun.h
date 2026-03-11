#ifndef FUN_H
#define FUN_H

#include <orca/discord.h>
#include "database.h"

void fun_module_init(struct discord *client, Database *db);
void register_fun_commands(struct discord *client,
                           u64_snowflake_t application_id,
                           u64_snowflake_t guild_id);
void on_fun_interaction(struct discord *client,
                        const struct discord_interaction *event);

void fun_track_message(u64_snowflake_t channel_id, const char *channel_name);

#endif /* FUN_H */