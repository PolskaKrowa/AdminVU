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
static HttpServer   g_http_server = { 0 };

static u64_snowflake_t g_bot_guild_id = 0;          /* single admin/infra guild */

#define MAX_DEV_GUILDS 4
static u64_snowflake_t g_dev_guild_ids[MAX_DEV_GUILDS];
static int             g_dev_guild_count = 0;

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

/*
 * Registers commands that should only exist in the bot's own
 * infrastructure guild — central admin, pairing, oversight, etc.
 */
static void register_admin_commands(struct discord  *client,
                                    u64_snowflake_t  application_id,
                                    u64_snowflake_t  bot_guild_id) {
    register_propagation_admin_commands(client, application_id, bot_guild_id);
    /* Add any other bot-team-only commands here. */
    printf("[main] Admin commands registered in bot guild %" PRIu64 "\n",
           bot_guild_id);
}

/*
 * Registers all community-facing commands globally.
 * Called once — applies to every guild the bot is in or ever joins.
 * Pass guild_id = 0 to the underlying registration calls to make them global.
 */
static void register_global_commands(struct discord  *client,
                                     u64_snowflake_t  application_id) {
    register_moderation_commands(client, application_id, 0);
    register_ticket_commands(client, application_id, 0);
    register_propagation_commands(client, application_id, 0);
    register_fun_commands(client, application_id, 0);
    /* /ping etc. */
    printf("[main] Global community commands registered.\n");
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

    /* Always register global community commands once. */
    register_global_commands(a->client, a->application_id);

    /* Register privileged admin commands only in the bot's own guild. */
    if (g_bot_guild_id)
        register_admin_commands(a->client, a->application_id, g_bot_guild_id);

    /*
     * Optionally re-register community commands as guild-scoped too on
     * dev guilds — useful during development for instant propagation,
     * since global commands can take up to an hour to update.
     */
    for (int i = 0; i < a->guild_count; i++) {
        register_moderation_commands(a->client, a->application_id, a->guild_ids[i]);
        register_ticket_commands(a->client, a->application_id, a->guild_ids[i]);
        register_propagation_commands(a->client, a->application_id, a->guild_ids[i]);
        register_fun_commands(a->client, a->application_id, a->guild_ids[i]);
        printf("[main] Dev guild %" PRIu64 " commands registered.\n",
               a->guild_ids[i]);
    }

    free(a);
    return NULL;
}

/* ── Event handlers ──────────────────────────────────────────────────────── */

void on_ready(struct discord *client) {
    printf("Bot is ready!\n");

    struct discord_user *bot = discord_get_self(client);
    u64_snowflake_t application_id = bot ? bot->id : 0;
    if (!application_id) {
        fprintf(stderr, "[main] Could not obtain application_id.\n");
        return;
    }

    factcheck_module_init(client);

    RegisterArgs *args = malloc(sizeof *args);
    if (!args) return;
    args->client         = client;
    args->application_id = application_id;
    args->guild_count    = g_dev_guild_count;
    memcpy(args->guild_ids, g_dev_guild_ids,
           (size_t)g_dev_guild_count * sizeof(u64_snowflake_t));

    pthread_t tid;
    if (pthread_create(&tid, NULL, register_commands_thread, args) != 0) {
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

    } else if (strcmp(cmd, "propagate", 9) == 0) {
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

    /* Bot's own infrastructure guild */
    const char *bot_guild_str = get_env("DISCORD_BOT_GUILD_ID");
    if (!bot_guild_str) {
        fprintf(stderr, "Error: DISCORD_BOT_GUILD_ID not set\n");
        cleanup_env();
        return EXIT_FAILURE;
    }
    g_bot_guild_id = parse_snowflake(bot_guild_str);

    /* Optional dev guilds for instant command updates */
    const char *dev_guild_str = get_env("DISCORD_DEV_GUILD_IDS");
    if (dev_guild_str)
        g_dev_guild_count = parse_guild_ids(dev_guild_str, g_dev_guild_ids, MAX_DEV_GUILDS);

    /* Module init — bot guild is the only fixed reference point */
    propagation_module_init(client, &g_database, g_bot_guild_id);

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

    /* Module init — bot guild is the single fixed reference point.
    * Community commands are registered globally inside on_ready()
    * and apply automatically to every guild the bot is in. */
    printf("Initialising modules...\n");
    ping_module_init(client, g_bot_guild_id);
    moderation_module_init(client, &g_database, g_bot_guild_id, token);
    ticket_module_init(client, &g_database);
    propagation_module_init(client, &g_database, g_bot_guild_id);
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