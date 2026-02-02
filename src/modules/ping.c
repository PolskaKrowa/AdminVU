#include "ping.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <ctype.h>

extern uint64_t fast_hash(const char *str, size_t len);

void on_ping_command(struct discord *client,
                     const struct discord_message *msg) {
    if (!msg->content) return;
    if (msg->author->bot) return;

    // 1. Strict Command Check
    // Checks if the string is exactly "!ping" or starts with "!ping "
    const char *cmd = "!ping";
    size_t cmd_len = strlen(cmd);
    
    if (strncmp(msg->content, cmd, cmd_len) != 0) return;
    
    // Ensure it's not "!pinganythingelse"
    if (msg->content[cmd_len] != '\0' && !isspace(msg->content[cmd_len])) {
        return;
    }

    // 2. Parse Input (the "target")
    // Skip "!ping" and any leading whitespace to find the argument
    const char *target = msg->content + cmd_len;
    while (*target && isspace(*target)) {
        target++;
    }

    // If no input was provided, default to the author's username
    if (*target == '\0') {
        target = msg->author->username;
    }

    printf("Received ping command from %s, targeting: %s\n", 
           msg->author->username, target);
    
    // 3. Hash the target (either the input or the username)
    uint64_t hash = fast_hash(target, strlen(target));
    
    char response[256];
    snprintf(response, sizeof(response), 
             "Pong! 🏓 (Target: %s, Hash: 0x%016"PRIx64")", target, hash);
    
    struct discord_create_message_params params = { .content = response,
               .message_reference = !msg->referenced_message
                   ? NULL
                   : &(struct discord_message_reference){
                     .message_id = msg->referenced_message->id,
                     .channel_id = msg->channel_id,
                     .guild_id = msg->guild_id,
                   } };

    discord_create_message(client, msg->channel_id, &params, NULL);                   

}

void ping_module_init(struct discord *client) {
    printf("Ping module initialised\n");
    discord_set_on_message_create(client, &on_ping_command);
}