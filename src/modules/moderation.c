#include "moderation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>

// Module-private state
static Database *g_db = NULL;
static u64_snowflake_t g_guild_id = 0;

// Helper: Check if user has moderation permissions
static bool has_mod_permissions(const struct discord_interaction *event) {
    if (!event->member || !event->member->permissions) return false;
    
    // Parse permissions string to uint64
    uint64_t perms = strtoull(event->member->permissions, NULL, 10);
    
    // Check for KICK_MEMBERS, BAN_MEMBERS, or MODERATE_MEMBERS permissions
    uint64_t required = DISCORD_PERMISSION_KICK_MEMBERS | 
                       DISCORD_PERMISSION_BAN_MEMBERS | 
                       DISCORD_PERMISSION_MODERATE_MEMBERS;
    
    return (perms & required) != 0;
}

// Helper: Get option value by name
static const char* get_option_value(const struct discord_interaction *event, 
                                    const char *name) {
    if (!event->data || !event->data->options) return NULL;
    
    for (int i = 0; event->data->options[i]; i++) {
        if (strcmp(event->data->options[i]->name, name) == 0) {
            return event->data->options[i]->value;
        }
    }
    return NULL;
}

// Helper: Send ephemeral response
static void send_ephemeral(struct discord *client,
                           const struct discord_interaction *event,
                           const char *message) {
    struct discord_interaction_response response = {
        .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
        .data = &(struct discord_interaction_callback_data){
            .content = (char *)message,
            .flags = 1 << 6,  // 64 = ephemeral flag
        },
    };
    
    discord_create_interaction_response(client, event->id, event->token, 
                                       &response, NULL);
}

// /warn command handler
static void handle_warn_command(struct discord *client,
                                const struct discord_interaction *event) {
    if (!has_mod_permissions(event)) {
        send_ephemeral(client, event, "❌ You don't have permission to warn members.");
        return;
    }
    
    const char *user_id_str = get_option_value(event, "user");
    const char *reason = get_option_value(event, "reason");
    
    if (!user_id_str) {
        send_ephemeral(client, event, "❌ User not specified.");
        return;
    }
    
    u64_snowflake_t target_id = (u64_snowflake_t)strtoull(user_id_str, NULL, 10);
    u64_snowflake_t moderator_id = event->member ? event->member->user->id : 0;
    
    // Add warning to database
    int result = db_add_warning(g_db, target_id, event->guild_id, 
                                moderator_id, reason);
    
    if (result != 0) {
        send_ephemeral(client, event, "❌ Failed to add warning to database.");
        return;
    }
    
    // Log the action
    db_log_action(g_db, MOD_ACTION_WARN, target_id, event->guild_id,
                  moderator_id, reason);
    
    // Get total warning count
    int warning_count = db_get_warning_count(g_db, target_id, event->guild_id);
    
    // Build response
    char response[512];
    snprintf(response, sizeof(response),
             "⚠️ User <@%" PRIu64 "> has been warned.\n"
             "**Reason:** %s\n"
             "**Total warnings:** %d",
             target_id, reason ? reason : "No reason provided", warning_count);
    
    struct discord_interaction_response resp = {
        .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
        .data = &(struct discord_interaction_callback_data){
            .content = response,
        },
    };
    
    discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
}

// /warnings command handler
static void handle_warnings_command(struct discord *client,
                                    const struct discord_interaction *event) {
    if (!has_mod_permissions(event)) {
        send_ephemeral(client, event, "❌ You don't have permission to view warnings.");
        return;
    }
    
    const char *user_id_str = get_option_value(event, "user");
    
    if (!user_id_str) {
        send_ephemeral(client, event, "❌ User not specified.");
        return;
    }
    
    u64_snowflake_t target_id = (u64_snowflake_t)strtoull(user_id_str, NULL, 10);
    
    Warning *warnings = NULL;
    int count = 0;
    
    int result = db_get_warnings(g_db, target_id, event->guild_id, &warnings, &count);
    
    if (result != 0) {
        send_ephemeral(client, event, "❌ Failed to retrieve warnings from database.");
        return;
    }
    
    char response[2000];
    if (count == 0) {
        snprintf(response, sizeof(response),
                 "📋 User <@%" PRIu64 "> has no warnings.", target_id);
    } else {
        snprintf(response, sizeof(response),
                 "📋 **Warnings for <@%" PRIu64 ">** (Total: %d)\n\n", 
                 target_id, count);
        
        for (int i = 0; i < count && i < 10; i++) {  // Limit to 10 most recent
            char temp[256];
            time_t ts = (time_t)warnings[i].timestamp;
            struct tm *tm_info = gmtime(&ts);
            char time_buf[64];
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S UTC", tm_info);
            
            snprintf(temp, sizeof(temp),
                     "**%d.** %s\n   By: <@%" PRIu64 ">\n   Reason: %s\n\n",
                     i + 1, time_buf, warnings[i].moderator_id,
                     warnings[i].reason ? warnings[i].reason : "No reason");
            strncat(response, temp, sizeof(response) - strlen(response) - 1);
        }
        
        if (count > 10) {
            char more[64];
            snprintf(more, sizeof(more), "*...and %d more warnings*", count - 10);
            strncat(response, more, sizeof(response) - strlen(response) - 1);
        }
    }
    
    db_free_warnings(warnings, count);
    
    send_ephemeral(client, event, response);
}

// /kick command handler
static void handle_kick_command(struct discord *client,
                                const struct discord_interaction *event) {
    if (!has_mod_permissions(event)) {
        send_ephemeral(client, event, "❌ You don't have permission to kick members.");
        return;
    }
    
    const char *user_id_str = get_option_value(event, "user");
    const char *reason = get_option_value(event, "reason");
    
    if (!user_id_str) {
        send_ephemeral(client, event, "❌ User not specified.");
        return;
    }
    
    u64_snowflake_t target_id = (u64_snowflake_t)strtoull(user_id_str, NULL, 10);
    u64_snowflake_t moderator_id = event->member ? event->member->user->id : 0;
    
    // Attempt to kick the user
    ORCAcode code = discord_remove_guild_member(client, event->guild_id, target_id);
    
    if (code != ORCA_OK) {
        char error[256];
        snprintf(error, sizeof(error), 
                 "❌ Failed to kick user (ORCAcode %d). Check bot permissions.", code);
        send_ephemeral(client, event, error);
        return;
    }
    
    // Log the action
    db_log_action(g_db, MOD_ACTION_KICK, target_id, event->guild_id,
                  moderator_id, reason);
    
    char response[512];
    snprintf(response, sizeof(response),
             "👢 User <@%" PRIu64 "> has been kicked.\n**Reason:** %s",
             target_id, reason ? reason : "No reason provided");
    
    struct discord_interaction_response resp = {
        .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
        .data = &(struct discord_interaction_callback_data){
            .content = response,
        },
    };
    
    discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
}

// /ban command handler
static void handle_ban_command(struct discord *client,
                               const struct discord_interaction *event) {
    if (!has_mod_permissions(event)) {
        send_ephemeral(client, event, "❌ You don't have permission to ban members.");
        return;
    }
    
    const char *user_id_str = get_option_value(event, "user");
    const char *reason = get_option_value(event, "reason");
    
    if (!user_id_str) {
        send_ephemeral(client, event, "❌ User not specified.");
        return;
    }
    
    u64_snowflake_t target_id = (u64_snowflake_t)strtoull(user_id_str, NULL, 10);
    u64_snowflake_t moderator_id = event->member ? event->member->user->id : 0;
    
    // Ban the user (delete 1 day of messages)
    ORCAcode code = discord_create_guild_ban(client, event->guild_id, target_id, reason);
    
    if (code != ORCA_OK) {
        char error[256];
        snprintf(error, sizeof(error), 
                 "❌ Failed to ban user (ORCAcode %d). Check bot permissions.", code);
        send_ephemeral(client, event, error);
        return;
    }
    
    // Log the action
    db_log_action(g_db, MOD_ACTION_BAN, target_id, event->guild_id,
                  moderator_id, reason);
    
    char response[512];
    snprintf(response, sizeof(response),
             "🔨 User <@%" PRIu64 "> has been banned.\n**Reason:** %s",
             target_id, reason ? reason : "No reason provided");
    
    struct discord_interaction_response resp = {
        .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
        .data = &(struct discord_interaction_callback_data){
            .content = response,
        },
    };
    
    discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
}

// /timeout command handler (timeout = mute in Discord API terms)
// NOTE: This requires Orca support for communication_disabled_until field
// If your Orca version doesn't support this, you'll need to implement role-based muting
static void handle_timeout_command(struct discord *client,
                                   const struct discord_interaction *event) {
    if (!has_mod_permissions(event)) {
        send_ephemeral(client, event, "❌ You don't have permission to timeout members.");
        return;
    }
    
    const char *user_id_str = get_option_value(event, "user");
    const char *duration_str = get_option_value(event, "duration");
    const char *reason = get_option_value(event, "reason");
    
    if (!user_id_str) {
        send_ephemeral(client, event, "❌ User not specified.");
        return;
    }
    
    u64_snowflake_t target_id = (u64_snowflake_t)strtoull(user_id_str, NULL, 10);
    u64_snowflake_t moderator_id = event->member ? event->member->user->id : 0;
    
    // Parse duration (in minutes)
    int duration_minutes = duration_str ? atoi(duration_str) : 10;
    if (duration_minutes < 1) duration_minutes = 10;
    if (duration_minutes > 40320) duration_minutes = 40320;  // Max 28 days
    
    // Log the action in database
    db_log_action(g_db, MOD_ACTION_MUTE, target_id, event->guild_id,
                  moderator_id, reason);
    
    // Note: The actual timeout implementation depends on your Orca version
    // Newer versions support communication_disabled_until in modify_guild_member_params
    // Older versions may require role-based muting
    
    send_ephemeral(client, event, 
                  "⚠️ Timeout feature requires Orca support for communication_disabled_until.\n"
                  "Please implement role-based muting or upgrade Orca to use Discord's native timeout.");
    
    /* Original implementation for newer Orca versions:
    
    time_t now = time(NULL);
    time_t timeout_until = now + (duration_minutes * 60);
    struct tm *tm_info = gmtime(&timeout_until);
    char iso_time[64];
    strftime(iso_time, sizeof(iso_time), "%Y-%m-%dT%H:%M:%S.000Z", tm_info);
    
    struct discord_modify_guild_member_params params = {
        .communication_disabled_until = iso_time,  // May not exist in older Orca
    };
    
    ORCAcode code = discord_modify_guild_member(client, event->guild_id, target_id,
                                                &params, NULL);
    
    if (code != ORCA_OK) {
        char error[256];
        snprintf(error, sizeof(error), 
                 "❌ Failed to timeout user (ORCAcode %d). Check bot permissions.", code);
        send_ephemeral(client, event, error);
        return;
    }
    
    char response[512];
    snprintf(response, sizeof(response),
             "🔇 User <@%" PRIu64 "> has been timed out for %d minutes.\n**Reason:** %s",
             target_id, duration_minutes, reason ? reason : "No reason provided");
    
    struct discord_interaction_response resp = {
        .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
        .data = &(struct discord_interaction_callback_data){
            .content = response,
        },
    };
    
    discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
    */
}

// Main interaction router
void on_moderation_interaction(struct discord *client,
                               const struct discord_interaction *event) {
    if (!event->data) return;
    if (event->type != DISCORD_INTERACTION_APPLICATION_COMMAND) return;
    
    const char *cmd = event->data->name;
    
    if (strcmp(cmd, "warn") == 0) {
        handle_warn_command(client, event);
    } else if (strcmp(cmd, "warnings") == 0) {
        handle_warnings_command(client, event);
    } else if (strcmp(cmd, "kick") == 0) {
        handle_kick_command(client, event);
    } else if (strcmp(cmd, "ban") == 0) {
        handle_ban_command(client, event);
    } else if (strcmp(cmd, "timeout") == 0) {
        handle_timeout_command(client, event);
    }
}

// Register all moderation commands
void register_moderation_commands(struct discord *client,
                                   u64_snowflake_t application_id,
                                   u64_snowflake_t guild_id) {
    // /warn command
    struct discord_application_command_option warn_user = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_USER,
        .name = "user",
        .description = "The user to warn",
        .required = true,
    };
    
    struct discord_application_command_option warn_reason = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name = "reason",
        .description = "Reason for the warning",
        .required = false,
    };
    
    struct discord_application_command_option *warn_opts[] = {
        &warn_user, &warn_reason, NULL
    };
    
    struct discord_create_guild_application_command_params warn_params = {
        .type = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name = "warn",
        .description = "Issue a warning to a user",
        .options = warn_opts,
    };
    
    discord_create_guild_application_command(client, application_id, guild_id,
                                             &warn_params, NULL);
    
    // /warnings command
    struct discord_application_command_option warnings_user = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_USER,
        .name = "user",
        .description = "The user to check warnings for",
        .required = true,
    };
    
    struct discord_application_command_option *warnings_opts[] = {
        &warnings_user, NULL
    };
    
    struct discord_create_guild_application_command_params warnings_params = {
        .type = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name = "warnings",
        .description = "View all warnings for a user",
        .options = warnings_opts,
    };
    
    discord_create_guild_application_command(client, application_id, guild_id,
                                             &warnings_params, NULL);
    
    // /kick command
    struct discord_application_command_option kick_user = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_USER,
        .name = "user",
        .description = "The user to kick",
        .required = true,
    };
    
    struct discord_application_command_option kick_reason = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name = "reason",
        .description = "Reason for kicking",
        .required = false,
    };
    
    struct discord_application_command_option *kick_opts[] = {
        &kick_user, &kick_reason, NULL
    };
    
    struct discord_create_guild_application_command_params kick_params = {
        .type = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name = "kick",
        .description = "Kick a user from the server",
        .options = kick_opts,
    };
    
    discord_create_guild_application_command(client, application_id, guild_id,
                                             &kick_params, NULL);
    
    // /ban command
    struct discord_application_command_option ban_user = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_USER,
        .name = "user",
        .description = "The user to ban",
        .required = true,
    };
    
    struct discord_application_command_option ban_reason = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name = "reason",
        .description = "Reason for banning",
        .required = false,
    };
    
    struct discord_application_command_option *ban_opts[] = {
        &ban_user, &ban_reason, NULL
    };
    
    struct discord_create_guild_application_command_params ban_params = {
        .type = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name = "ban",
        .description = "Ban a user from the server",
        .options = ban_opts,
    };
    
    discord_create_guild_application_command(client, application_id, guild_id,
                                             &ban_params, NULL);
    
    // /timeout command
    struct discord_application_command_option timeout_user = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_USER,
        .name = "user",
        .description = "The user to timeout",
        .required = true,
    };
    
    struct discord_application_command_option timeout_duration = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_INTEGER,
        .name = "duration",
        .description = "Duration in minutes (default: 10, max: 40320 = 28 days)",
        .required = false,
    };
    
    struct discord_application_command_option timeout_reason = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name = "reason",
        .description = "Reason for timeout",
        .required = false,
    };
    
    struct discord_application_command_option *timeout_opts[] = {
        &timeout_user, &timeout_duration, &timeout_reason, NULL
    };
    
    struct discord_create_guild_application_command_params timeout_params = {
        .type = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name = "timeout",
        .description = "Timeout a user (prevent them from sending messages)",
        .options = timeout_opts,
    };
    
    discord_create_guild_application_command(client, application_id, guild_id,
                                             &timeout_params, NULL);
    
    printf("[moderation] All moderation commands registered\n");
}

void moderation_module_init(struct discord *client, Database *db, u64_snowflake_t guild_id) {
    g_db = db;
    g_guild_id = guild_id;
    
    // The interaction handler is shared, so we just set a flag
    printf("[moderation] Moderation module initialised\n");
}