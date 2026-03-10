#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <inttypes.h>
#include <orca/discord.h>
#include <pthread.h>

#include "env_parser.h"
#include "database.h"
#include "modules/ping.h"
#include "modules/moderation.h"
#include "modules/ticket.h"
#include "modules/factcheck.h"
#include "modules/propagation.h"

// Global client pointer for signal handling
static struct discord *g_client = NULL;
static Database g_database = { 0 };

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

/* ---------------------------------------------------------------------------
 * register_slash_commands
 *
 * Registers guild-scoped slash commands for all modules.
 * --------------------------------------------------------------------------- */
void register_slash_commands(struct discord *client,
                             u64_snowflake_t application_id,
                             u64_snowflake_t guild_id) {
    /* The "target" option – a USER picker, not required. */
    static struct discord_application_command_option target_option = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_USER,
        .name        = "target",
        .description = "Member to hash (defaults to you)",
        .required    = false,
    };

    /* Orca expects a NULL-terminated array of pointers. */
    static struct discord_application_command_option *options[] = {
        &target_option,
        NULL,
    };

    static struct discord_create_guild_application_command_params params = {
        .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name        = "ping",
        .description = "Pong! Optionally hash a target member's display name.",
        .options     = options,
    };

    ORCAcode code = discord_create_guild_application_command(
        client, application_id, guild_id, &params, NULL);

    if (code == ORCA_OK)
        printf("[ping] /ping slash command registered in guild %" PRIu64 "\n",
               guild_id);
    else
        printf("[ping] Failed to register /ping (ORCAcode %d)\n", code);

    // Register moderation commands
    register_moderation_commands(client, application_id, guild_id);

    // Register ticket commands (guild-specific ones)
    register_ticket_commands(client, application_id, guild_id);

    // Register propagation commands
    register_propagation_commands(client, application_id, guild_id);

    /* factcheck is triggered via plain messages, not slash commands – no
     * registration needed here. */
}

typedef struct {
    struct discord  *client;
    u64_snowflake_t  application_id;
    u64_snowflake_t  guild_id;
} RegisterArgs;

static void *register_commands_thread(void *arg) {
    RegisterArgs *a = arg;
    register_slash_commands(a->client, a->application_id, a->guild_id);
    free(a);
    return NULL;
}

// Combined interaction handler for all modules
void on_interaction_create_combined(struct discord *client,
                                    const struct discord_interaction *event) {
    if (!event->data) return;
    if (event->type != DISCORD_INTERACTION_APPLICATION_COMMAND) return;

    const char *cmd = event->data->name;

    printf("[main] Received interaction: %s\n", cmd);

    // Route to appropriate module
    if (strcmp(cmd, "ping") == 0) {
        on_ping_interaction(client, event);

    } else if (strcmp(cmd, "warn")     == 0 ||
               strcmp(cmd, "warnings") == 0 ||
               strcmp(cmd, "kick")     == 0 ||
               strcmp(cmd, "ban")      == 0 ||
               strcmp(cmd, "timeout")  == 0) {
        on_moderation_interaction(client, event);

    } else if (strcmp(cmd, "ticket")       == 0 ||
               strcmp(cmd, "closeticket")  == 0 ||
               strcmp(cmd, "ticketconfig") == 0) {
        on_ticket_interaction(client, event);

    } else if (strcmp(cmd, "propagate")         == 0 ||
               strcmp(cmd, "propagate-config")  == 0 ||
               strcmp(cmd, "propagate-opt-out") == 0 ||
               strcmp(cmd, "propagate-history") == 0 ||
               strcmp(cmd, "propagate-revoke")  == 0) {
        on_propagation_interaction(client, event);

    } else {
        printf("[main] Unknown command: %s\n", cmd);
    }
}

// Message handler – routes to all message-driven modules
void on_message_create(struct discord *client,
                       const struct discord_message *event) {
    // Ticket module – handles ticket channel messages
    on_ticket_message(client, event);

    // Factcheck module – handles "@bot is this true?" replies
    on_factcheck_message(client, event);
}

void on_ready(struct discord *client) {
    printf("Bot is ready!\n");

    struct discord_user *bot = discord_get_self(client);
    if (bot && bot->username)
        printf("Logged in as: %s\n", bot->username);

    u64_snowflake_t application_id = bot ? bot->id : 0;
    if (!application_id) {
        fprintf(stderr, "[main] Could not obtain application_id.\n");
        return;
    }

    factcheck_module_init(client);

    /* Spin up a background thread so the WebSocket heartbeat
     * is never starved by blocking REST calls.              */
    RegisterArgs *args = malloc(sizeof *args);
    args->client         = client;
    args->application_id = application_id;
    args->guild_id       = g_guild_id;

    pthread_t tid;
    if (pthread_create(&tid, NULL, register_commands_thread, args) != 0) {
        fprintf(stderr, "[main] Failed to spawn registration thread.\n");
        free(args);
        return;
    }
    pthread_detach(tid);   /* fire-and-forget; thread frees its own args */
}

static void on_guild_create_handler(struct discord *client,
                                    const struct discord_guild *guild) {
    if (guild && guild->id)
        propagation_on_guild_register(guild->id);
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

    // Get guild ID
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

    // Initialise database
    printf("Initialising database...\n");
    if (db_init(&g_database, "bot_data.db") != 0) {
        fprintf(stderr, "Error: Failed to initialise database\n");
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
        db_cleanup(&g_database);
        cleanup_env();
        return EXIT_FAILURE;
    }

    g_client = client;

    // Set up event handlers
    discord_set_on_ready(client, (void*)&on_ready);
    discord_set_on_interaction_create(client, &on_interaction_create_combined);
    discord_set_on_message_create(client, &on_message_create);
    discord_set_on_guild_create(client, on_guild_create_handler);

    // Initialise modules
    printf("Initialising modules...\n");
    ping_module_init(client, g_guild_id);
    moderation_module_init(client, &g_database, g_guild_id, token);
    ticket_module_init(client, &g_database);
    propagation_module_init(client, &g_database, g_guild_id);

    // Factcheck module – early init so the bot ID can be captured before
    // the first message arrives.  It will be re-initialised in on_ready()
    // once the bot is properly connected and the ID is guaranteed valid.
    factcheck_module_init(client);

    // Start the bot
    printf("Starting bot...\n");
    discord_run(client);

    // Cleanup
    discord_cleanup(client);
    db_cleanup(&g_database);
    cleanup_env();
    printf("Bot shut down successfully\n");

    return EXIT_SUCCESS;
}