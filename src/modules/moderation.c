#include "moderation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <curl/curl.h>

// Module-private state
static Database        *g_db       = NULL;
static u64_snowflake_t  g_guild_id = 0;
static char             g_bot_token[256] = { 0 };

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
    struct discord_create_guild_ban_params params = {
        .delete_message_days = 1
    };
    
    ORCAcode code = discord_create_guild_ban(client, event->guild_id, target_id, &params);
    
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

/* ---------------------------------------------------------------------------
 * patch_member_timeout
 *
 * Issues a PATCH /guilds/{guild}/members/{user} directly via libcurl,
 * setting communication_disabled_until to the supplied ISO 8601 string.
 * This bypasses Orca's typed struct layer, which does not expose that field
 * in this build.
 *
 * Returns ORCA_OK (0) on HTTP 2xx, ORCA_CURLE_INTERNAL otherwise.
 * --------------------------------------------------------------------------- */
static ORCAcode patch_member_timeout(u64_snowflake_t guild_id,
                                      u64_snowflake_t user_id,
                                      const char     *iso_timestamp) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "[moderation] curl_easy_init() failed\n");
        return ORCA_CURLE_INTERNAL;
    }

    char url[256];
    snprintf(url, sizeof(url),
             "https://discord.com/api/v10/guilds/%" PRIu64 "/members/%" PRIu64,
             guild_id, user_id);

    char body[256];
    snprintf(body, sizeof(body),
             "{\"communication_disabled_until\":\"%s\"}", iso_timestamp);

    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bot %s", g_bot_token);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
    /* Silence response body – we only care about the status code. */
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                     (curl_write_callback)(void *)fwrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, stderr);

    CURLcode res      = curl_easy_perform(curl);
    long     http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "[moderation] curl error: %s\n", curl_easy_strerror(res));
        return ORCA_CURLE_INTERNAL;
    }
    if (http_code < 200 || http_code >= 300) {
        fprintf(stderr, "[moderation] Discord returned HTTP %ld for timeout PATCH\n",
                http_code);
        return ORCA_CURLE_INTERNAL;
    }

    return ORCA_OK;
}


/* Times out a member for a given duration (in minutes, default 10, max 40320
 * which equals 28 days — the Discord API ceiling).
 *
 * The timeout is persisted in the database via db_add_timeout() so the bot
 * can track active timeouts independently of Discord's own state.
 * --------------------------------------------------------------------------- */
static void handle_timeout_command(struct discord *client,
                                   const struct discord_interaction *event) {
    if (!has_mod_permissions(event)) {
        send_ephemeral(client, event, "❌ You don't have permission to timeout members.");
        return;
    }

    const char *user_id_str  = get_option_value(event, "user");
    const char *duration_str = get_option_value(event, "duration");
    const char *reason       = get_option_value(event, "reason");

    if (!user_id_str) {
        send_ephemeral(client, event, "❌ User not specified.");
        return;
    }

    /* Duration: default 10 minutes, capped at 40320 (28 days in minutes). */
    long duration_minutes = duration_str ? strtol(duration_str, NULL, 10) : 10;
    if (duration_minutes < 1)     duration_minutes = 1;
    if (duration_minutes > 40320) duration_minutes = 40320;

    u64_snowflake_t target_id    = (u64_snowflake_t)strtoull(user_id_str, NULL, 10);
    u64_snowflake_t moderator_id = event->member ? event->member->user->id : 0;

    /* Compute Unix expiry and ISO 8601 string for Discord's API. */
    time_t now       = time(NULL);
    time_t expires_at = now + (time_t)(duration_minutes * 60);
    struct tm *tm_info = gmtime(&expires_at);
    char iso_timestamp[32];
    strftime(iso_timestamp, sizeof(iso_timestamp), "%Y-%m-%dT%H:%M:%SZ", tm_info);

    /* Apply the timeout via Discord's modify-guild-member endpoint.
     * The communication_disabled_until field was added to Orca's struct in
     * later builds.  If your compiler flags it as missing, replace the struct
     * literal below with a discord_run_json() call. */
    ORCAcode code = patch_member_timeout(event->guild_id, target_id, iso_timestamp);
    if (code != ORCA_OK) {
        char error[256];
        snprintf(error, sizeof(error),
                 "❌ Failed to timeout user (ORCAcode %d). "
                 "Check bot permissions and that the target is below me in "
                 "the role hierarchy.",
                 code);
        send_ephemeral(client, event, error);
        return;
    }

    /* Persist to database so we can query / remove timeouts later. */
    db_add_timeout(g_db, target_id, event->guild_id,
                   moderator_id, reason, (long)expires_at);

    /* Write to the audit log. */
    db_log_action(g_db, MOD_ACTION_TIMEOUT, target_id, event->guild_id,
                  moderator_id, reason);

    char response[512];
    snprintf(response, sizeof(response),
             "🔇 <@%" PRIu64 "> has been timed out for **%ld minute%s**.\n"
             "**Reason:** %s\n"
             "**Expires:** %s",
             target_id,
             duration_minutes, duration_minutes == 1 ? "" : "s",
             reason ? reason : "No reason provided",
             iso_timestamp);

    struct discord_interaction_response resp = {
        .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
        .data = &(struct discord_interaction_callback_data){
            .content = response,
        },
    };

    discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
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
    static struct discord_application_command_option warn_user = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_USER,
        .name = "user",
        .description = "The user to warn",
        .required = true,
    };
    static struct discord_application_command_option warn_reason = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name = "reason",
        .description = "Reason for the warning",
        .required = false,
    };
    static struct discord_application_command_option *warn_opts[] = {
        &warn_user, &warn_reason, NULL
    };
    static struct discord_create_guild_application_command_params warn_params = {
        .type = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name = "warn",
        .description = "Issue a warning to a user",
        .options = warn_opts,
    };
    discord_create_guild_application_command(client, application_id, guild_id,
                                             &warn_params, NULL);

    // /warnings command
    static struct discord_application_command_option warnings_user = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_USER,
        .name = "user",
        .description = "The user to check warnings for",
        .required = true,
    };
    static struct discord_application_command_option *warnings_opts[] = {
        &warnings_user, NULL
    };
    static struct discord_create_guild_application_command_params warnings_params = {
        .type = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name = "warnings",
        .description = "View all warnings for a user",
        .options = warnings_opts,
    };
    discord_create_guild_application_command(client, application_id, guild_id,
                                             &warnings_params, NULL);

    // /kick command
    static struct discord_application_command_option kick_user = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_USER,
        .name = "user",
        .description = "The user to kick",
        .required = true,
    };
    static struct discord_application_command_option kick_reason = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name = "reason",
        .description = "Reason for kicking",
        .required = false,
    };
    static struct discord_application_command_option *kick_opts[] = {
        &kick_user, &kick_reason, NULL
    };
    static struct discord_create_guild_application_command_params kick_params = {
        .type = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name = "kick",
        .description = "Kick a user from the server",
        .options = kick_opts,
    };
    discord_create_guild_application_command(client, application_id, guild_id,
                                             &kick_params, NULL);

    // /ban command
    static struct discord_application_command_option ban_user = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_USER,
        .name = "user",
        .description = "The user to ban",
        .required = true,
    };
    static struct discord_application_command_option ban_reason = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name = "reason",
        .description = "Reason for banning",
        .required = false,
    };
    static struct discord_application_command_option *ban_opts[] = {
        &ban_user, &ban_reason, NULL
    };
    static struct discord_create_guild_application_command_params ban_params = {
        .type = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name = "ban",
        .description = "Ban a user from the server",
        .options = ban_opts,
    };
    discord_create_guild_application_command(client, application_id, guild_id,
                                             &ban_params, NULL);

    // /timeout command
    static struct discord_application_command_option timeout_user = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_USER,
        .name = "user",
        .description = "The user to timeout",
        .required = true,
    };
    static struct discord_application_command_option timeout_duration = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_INTEGER,
        .name = "duration",
        .description = "Duration in minutes (default: 10, max: 40320 = 28 days)",
        .required = false,
    };
    static struct discord_application_command_option timeout_reason = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name = "reason",
        .description = "Reason for timeout",
        .required = false,
    };
    static struct discord_application_command_option *timeout_opts[] = {
        &timeout_user, &timeout_duration, &timeout_reason, NULL
    };
    static struct discord_create_guild_application_command_params timeout_params = {
        .type = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name = "timeout",
        .description = "Timeout a user (prevent them from sending messages)",
        .options = timeout_opts,
    };
    discord_create_guild_application_command(client, application_id, guild_id,
                                             &timeout_params, NULL);

    printf("[moderation] All moderation commands registered\n");
}

void moderation_module_init(struct discord *client, Database *db,
                             u64_snowflake_t guild_id, const char *bot_token) {
    g_db = db;
    g_guild_id = guild_id;
    if (bot_token)
        snprintf(g_bot_token, sizeof(g_bot_token), "%s", bot_token);

    printf("[moderation] Moderation module initialised\n");
}