#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <inttypes.h>
#include <pthread.h>
#include <orca/discord.h>

#include "env_parser.h"
#include "database.h"
#include "http_server.h"
#include "modules/ping.h"
#include "modules/moderation.h"
#include "modules/ticket.h"
#include "modules/factcheck.h"
#include "modules/propagation.h"
#include "modules/fun.h"

/* ── Global state ─────────────────────────────────────────────────────────── */

static struct discord *g_client   = NULL;
static Database        g_database = { 0 };
static HttpServer g_http_server = { 0 };

/* Parsed guild IDs from DISCORD_GUILD_ID (comma-separated). */
#define MAX_GUILDS 64
static u64_snowflake_t g_guild_ids[MAX_GUILDS];
static int             g_guild_count = 0;

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static u64_snowflake_t parse_snowflake(const char *str) {
    if (!str) return 0;
    return (u64_snowflake_t)strtoull(str, NULL, 10);
}

/*
 * parse_guild_ids
 *
 * Accepts a comma-separated string such as "123456,789012,345678" and
 * populates g_guild_ids / g_guild_count.  Whitespace around commas is
 * trimmed so "123, 456 , 789" works too.
 */
static int parse_guild_ids(const char *raw) {
    if (!raw || raw[0] == '\0') return 0;

    /* Work on a mutable copy. */
    char *buf = strdup(raw);
    if (!buf) return 0;

    int count = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(buf, ",", &saveptr);

    while (tok && count < MAX_GUILDS) {
        /* Trim leading whitespace. */
        while (*tok == ' ' || *tok == '\t') tok++;
        /* Trim trailing whitespace. */
        char *end = tok + strlen(tok) - 1;
        while (end > tok && (*end == ' ' || *end == '\t')) *end-- = '\0';

        if (*tok != '\0') {
            u64_snowflake_t id = parse_snowflake(tok);
            if (id != 0) {
                g_guild_ids[count++] = id;
            } else {
                fprintf(stderr, "[main] Ignoring invalid guild ID: \"%s\"\n", tok);
            }
        }
        tok = strtok_r(NULL, ",", &saveptr);
    }

    free(buf);
    return count;
}

/* ── Signal handling ─────────────────────────────────────────────────────── */

void signal_handler(int signum) {
    printf("\nReceived signal %d, shutting down...\n", signum);
    if (g_client)
        discord_shutdown(g_client);
}

/* ── Slash-command registration ──────────────────────────────────────────── */

void register_slash_commands(struct discord *client,
                             u64_snowflake_t application_id,
                             u64_snowflake_t guild_id) {
    static struct discord_application_command_option target_option = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_USER,
        .name        = "target",
        .description = "Member to hash (defaults to you)",
        .required    = false,
    };
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
        printf("[ping] /ping registered in guild %" PRIu64 "\n", guild_id);
    else
        printf("[ping] Failed to register /ping in guild %" PRIu64
               " (ORCAcode %d)\n", guild_id, code);

    register_moderation_commands(client, application_id, guild_id);
    register_ticket_commands(client, application_id, guild_id);
    register_propagation_commands(client, application_id, guild_id);
    register_fun_commands(client, application_id, guild_id);
}

/* ── Background registration thread ─────────────────────────────────────── */

typedef struct {
    struct discord  *client;
    u64_snowflake_t  application_id;
    /* Snapshot of the guild list at the moment on_ready fires. */
    u64_snowflake_t  guild_ids[MAX_GUILDS];
    int              guild_count;
} RegisterArgs;

static void *register_commands_thread(void *arg) {
    RegisterArgs *a = arg;

    for (int i = 0; i < a->guild_count; i++) {
        printf("[main] Registering commands in guild %" PRIu64
               " (%d/%d)…\n", a->guild_ids[i], i + 1, a->guild_count);
        register_slash_commands(a->client, a->application_id, a->guild_ids[i]);
    }

    printf("[main] Command registration complete for %d guild(s).\n",
           a->guild_count);
    free(a);
    return NULL;
}

/* ── Event handlers ──────────────────────────────────────────────────────── */

void on_ready(struct discord *client) {
    printf("Bot is ready!\n");

    struct discord_user *bot = discord_get_self(client);
    if (bot && bot->username)
        printf("Logged in as: %s\n", bot->username);

    u64_snowflake_t application_id = bot ? bot->id : 0;
    if (!application_id) {
        fprintf(stderr, "[main] Could not obtain application_id – "
                        "slash commands will not be registered.\n");
        return;
    }

    factcheck_module_init(client);

    if (g_guild_count == 0) {
        fprintf(stderr, "[main] No valid guild IDs – "
                        "skipping command registration.\n");
        return;
    }

    /* Copy state into heap-allocated args so the thread owns its data. */
    RegisterArgs *args = malloc(sizeof *args);
    if (!args) {
        fprintf(stderr, "[main] OOM allocating RegisterArgs.\n");
        return;
    }
    args->client         = client;
    args->application_id = application_id;
    args->guild_count    = g_guild_count;
    memcpy(args->guild_ids, g_guild_ids,
           (size_t)g_guild_count * sizeof(u64_snowflake_t));

    pthread_t tid;
    if (pthread_create(&tid, NULL, register_commands_thread, args) != 0) {
        fprintf(stderr, "[main] Failed to spawn registration thread.\n");
        free(args);
        return;
    }
    pthread_detach(tid);
}

void on_interaction_create_combined(struct discord *client,
                                    const struct discord_interaction *event) {
    if (!event->data) return;

    /* Component interactions (e.g. trivia buttons) don't have a name –
     * route them to the fun module directly. */
    if (event->type == DISCORD_INTERACTION_MESSAGE_COMPONENT) {
        if (event->data->custom_id &&
            strncmp(event->data->custom_id, "trivia:", 7) == 0) {
            on_fun_interaction(client, event);
        }
        return;
    }

    if (event->type != DISCORD_INTERACTION_APPLICATION_COMMAND) return;

    const char *cmd = event->data->name;
    printf("[main] Received interaction: %s\n", cmd);

    if (strcmp(cmd, "ping") == 0) {
        on_ping_interaction(client, event);

    } else if (strcmp(cmd, "warn")     == 0 ||
               strcmp(cmd, "warnings") == 0 ||
               strcmp(cmd, "kick")     == 0 ||
               strcmp(cmd, "ban")      == 0 ||
               strcmp(cmd, "timeout")  == 0) {
        on_moderation_interaction(client, event);

    } else if (strcmp(cmd, "ticket")       == 0 ||
               strcmp(cmd, "ticketconfig") == 0) {
        on_ticket_interaction(client, event);

    } else if (strcmp(cmd, "propagate")         == 0 ||
               strcmp(cmd, "propagate-config")  == 0 ||
               strcmp(cmd, "propagate-opt-out") == 0 ||
               strcmp(cmd, "propagate-history") == 0 ||
               strcmp(cmd, "propagate-revoke")  == 0) {
        on_propagation_interaction(client, event);

    } else if (strcmp(cmd, "joke")     == 0 ||
               strcmp(cmd, "roll")     == 0 ||
               strcmp(cmd, "8ball")    == 0 ||
               strcmp(cmd, "choose")   == 0 ||
               strcmp(cmd, "coinflip") == 0 ||
               strcmp(cmd, "rps")      == 0 ||
               strcmp(cmd, "trivia")   == 0 ||
               strcmp(cmd, "activity") == 0) {
        on_fun_interaction(client, event);

    } else {
        printf("[main] Unknown command: %s\n", cmd);
    }
}

void on_message_create(struct discord *client,
                       const struct discord_message *event) {
    on_ticket_message(client, event);
    on_factcheck_message(client, event);

    /* Track channel activity for /activity */
    if (event->channel_id)
        fun_track_message(event->channel_id,
                          event->channel_id ? NULL : NULL);
}

static void on_guild_create_handler(struct discord *client,
                                    const struct discord_guild *guild) {
    if (guild && guild->id)
        propagation_on_guild_register(guild->id);
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    printf("Loading environment from ../.env\n");
    if (load_env_file("../.env") == 0)
        printf("Successfully loaded .env file\n");
    else
        printf("No .env file found, will use environment variables\n");

    /* Token */
    const char *token = get_env("DISCORD_BOT_TOKEN");
    if (!token) {
        fprintf(stderr, "Error: DISCORD_BOT_TOKEN not found\n");
        fprintf(stderr, "  1. Create a .env file with DISCORD_BOT_TOKEN=your_token\n");
        fprintf(stderr, "  2. Or set the DISCORD_BOT_TOKEN environment variable\n");
        cleanup_env();
        return EXIT_FAILURE;
    }

    /* Guild IDs – comma-separated, e.g. "123456789,987654321" */
    const char *guild_id_str = get_env("DISCORD_GUILD_ID");
    if (!guild_id_str) {
        fprintf(stderr, "Error: DISCORD_GUILD_ID not found\n");
        fprintf(stderr, "  Set one or more comma-separated guild IDs, e.g.:\n");
        fprintf(stderr, "  DISCORD_GUILD_ID=123456789,987654321\n");
        cleanup_env();
        return EXIT_FAILURE;
    }

    g_guild_count = parse_guild_ids(guild_id_str);
    if (g_guild_count == 0) {
        fprintf(stderr, "Error: DISCORD_GUILD_ID contained no valid IDs\n");
        cleanup_env();
        return EXIT_FAILURE;
    }

    printf("Registering commands in %d guild(s):\n", g_guild_count);
    for (int i = 0; i < g_guild_count; i++)
        printf("  [%d] %" PRIu64 "\n", i + 1, g_guild_ids[i]);

    /* Database */
    printf("Initialising database...\n");
    if (db_init(&g_database, "bot_data.db") != 0) {
        fprintf(stderr, "Error: Failed to initialise database\n");
        cleanup_env();
        return EXIT_FAILURE;
    }

    if (http_server_init(&g_http_server, 8080, "web", &g_database) == 0)
        http_server_start(&g_http_server);

    /* Signals */
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    /* Discord client */
    struct discord *client = discord_init(token);
    if (!client) {
        fprintf(stderr, "Error: Failed to initialise Discord client\n");
        db_cleanup(&g_database);
        cleanup_env();
        return EXIT_FAILURE;
    }
    g_client = client;

    /* Event hooks */
    discord_set_on_ready(client, (void *)&on_ready);
    discord_set_on_interaction_create(client, &on_interaction_create_combined);
    discord_set_on_message_create(client, &on_message_create);
    discord_set_on_guild_create(client, on_guild_create_handler);

    /* Module init – pass the first guild ID where a single value is needed;
     * command registration handles all guilds inside on_ready().          */
    printf("Initialising modules...\n");
    ping_module_init(client, g_guild_ids[0]);
    moderation_module_init(client, &g_database, g_guild_ids[0], token);
    ticket_module_init(client, &g_database);
    propagation_module_init(client, &g_database, g_guild_ids[0]);
    factcheck_module_init(client);
    fun_module_init(client, &g_database);

    /* Run */
    printf("Starting bot...\n");
    discord_run(client);

    /* Cleanup */
    discord_cleanup(client);
    db_cleanup(&g_database);
    cleanup_env();
    http_server_stop(&g_http_server);
    printf("Bot shut down successfully\n");

    return EXIT_SUCCESS;
}