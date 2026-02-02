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

// Signal handler for graceful shutdown
void signal_handler(int signum) {
    printf("\nReceived signal %d, shutting down...\n", signum);
    if (g_client) {
        discord_shutdown(g_client);
    }
}

// Ready event handler - simple version
void on_ready(struct discord *client) {
    printf("Bot is ready!\n");
    
    // Try to get bot info if the function exists
    struct discord_user *bot = discord_get_self(client);
    if (bot && bot->username) {
        printf("Logged in as: %s\n", bot->username);
    }
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

    // Initialize modules
    printf("Initialising modules...\n");
    ping_module_init(client);

    // Start the bot
    printf("Starting bot...\n");
    discord_run(client);

    // Cleanup
    discord_cleanup(client);
    cleanup_env();
    printf("Bot shut down successfully\n");

    return EXIT_SUCCESS;
}
