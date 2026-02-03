#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <inttypes.h>
#include <orca/discord.h>

#include "env_parser.h"
#include "modules/ping.h"

// Global client pointer for signal handling
static struct discord *g_client = NULL;

// Guild ID read from the environment at startup; used to register
// guild-scoped slash commands (instant propagation vs. up to 1 h for global).
static u64_snowflake_t g_guild_id = 0;

// Tiny helper: turn a decimal snowflake string into a u64
static u64_snowflake_t parse_snowflake(const char *str) {
    if (!str)
        return 0;
    return (u64_snowflake_t)strtoull(str, NULL, 10);
}

// Signal handler for graceful shutdown
void signal_handler(int signum) {
    printf("\nReceived signal %d, shutting down...\n", signum);
    if (g_client) {
        discord_shutdown(g_client);
    }
}

// Ready event handler
// This is the earliest point at which discord_get_self() is populated,
// so slash-command registration lives here.
void on_ready(struct discord *client) {
    printf("Bot is ready!\n");

    struct discord_user *bot = discord_get_self(client);
    if (bot && bot->username)
        printf("Logged in as: %s\n", bot->username);

    // For bot users, the user ID *is* the application ID.
    u64_snowflake_t application_id = bot ? bot->id : 0;
    if (!application_id) {
        fprintf(stderr, "[main] Could not obtain application_id – "
                        "slash commands will not be registered.\n");
        return;
    }

    // Now that we have the application_id, register slash commands.
    register_slash_commands(client, application_id, g_guild_id);
}

int main(int argc, char *argv[]) {
    // Try to load .env file from parent directory
    printf("Loading environment from ../.env\n");
    if (load_env_file("../.env") == 0) {
        printf("Successfully loaded .env file\n");
    } else {
        printf("No .env file found, will use environment variables\n");
    }

    // Get token from environment (checks both actual env and .env file)
    const char *token = get_env("DISCORD_BOT_TOKEN");
    if (!token) {
        fprintf(stderr, "Error: DISCORD_BOT_TOKEN not found\n");
        fprintf(stderr, "Please either:\n");
        fprintf(stderr, "  1. Create a .env file in the parent directory with DISCORD_BOT_TOKEN=your_token\n");
        fprintf(stderr, "  2. Set the DISCORD_BOT_TOKEN environment variable\n");
        cleanup_env();
        return EXIT_FAILURE;
    }

    // Get guild ID – slash commands are registered per-guild so that
    // they propagate instantly during development.
    const char *guild_id_str = get_env("DISCORD_GUILD_ID");
    if (!guild_id_str) {
        fprintf(stderr, "Error: DISCORD_GUILD_ID not found\n");
        fprintf(stderr, "Guild-scoped slash commands need a guild ID.  Please either:\n");
        fprintf(stderr, "  1. Add DISCORD_GUILD_ID=<id> to your .env file\n");
        fprintf(stderr, "  2. Set the DISCORD_GUILD_ID environment variable\n");
        fprintf(stderr, "  (Enable Developer Mode in Discord to copy your server ID)\n");
        cleanup_env();
        return EXIT_FAILURE;
    }
    g_guild_id = parse_snowflake(guild_id_str);
    printf("Target guild ID: %" PRIu64 "\n", g_guild_id);

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Create Discord client with the simplest API
    struct discord *client = discord_init(token);
    if (!client) {
        fprintf(stderr, "Error: Failed to initialise Discord client\n");
        cleanup_env();
        return EXIT_FAILURE;
    }
    
    g_client = client;

    // Set up event handlers
    discord_set_on_ready(client, (void*)&on_ready);

    // Initialise modules
    // ping_module_init binds on_interaction_create; the actual /ping
    // command registration is deferred to on_ready (needs application_id).
    printf("Initialising modules...\n");
    ping_module_init(client, g_guild_id);

    // Start the bot
    printf("Starting bot...\n");
    discord_run(client);

    // Cleanup
    discord_cleanup(client);
    cleanup_env();
    printf("Bot shut down successfully\n");

    return EXIT_SUCCESS;
}