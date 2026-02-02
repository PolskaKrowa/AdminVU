#ifndef PING_H
#define PING_H

#include <orca/discord.h>

// Initialize the ping module
void ping_module_init(struct discord *client);

// Handle ping command
void on_ping_command(struct discord *client,
                     const struct discord_message *msg);

#endif // PING_H
