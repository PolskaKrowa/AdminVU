#include "chess.h"
#include "modules/chess/src/chess_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include <pthread.h>
#include <time.h>

/* ---------------------------------------------------------------------------
 * Game state storage
 *
 * Each active game is stored with the key (channel_id << 32 | user_id).
 * This allows multiple games in the same server if they're in different
 * channels, but prevents two concurrent games in the same channel for the
 * same user.
 * --------------------------------------------------------------------------- */

#define MAX_GAMES 64

typedef struct {
    uint64_t key;           /* (channel_id << 32) | user_id                */
    ChessEngine* engine;
    int human_side;         /* CE_WHITE or CE_BLACK                         */
    bool active;
} GameState;

static GameState g_games[MAX_GAMES];
static int g_game_count = 0;
static pthread_mutex_t g_games_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct discord *g_client = NULL;

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------- */

static uint64_t game_key(u64_snowflake_t channel_id, u64_snowflake_t user_id) {
    return ((uint64_t)channel_id << 32) | (uint64_t)user_id;
}

static int find_game(uint64_t key) {
    for (int i = 0; i < g_game_count; i++) {
        if (g_games[i].key == key && g_games[i].active)
            return i;
    }
    return -1;
}

static GameState* create_game(uint64_t key) {
    pthread_mutex_lock(&g_games_mutex);

    int idx = find_game(key);
    if (idx >= 0) {
        /* Game already exists */
        pthread_mutex_unlock(&g_games_mutex);
        return &g_games[idx];
    }

    if (g_game_count >= MAX_GAMES) {
        pthread_mutex_unlock(&g_games_mutex);
        return NULL;
    }

    idx = g_game_count++;
    g_games[idx].key = key;
    g_games[idx].engine = ce_create(NULL);
    g_games[idx].active = true;

    pthread_mutex_unlock(&g_games_mutex);
    return &g_games[idx];
}

static void delete_game(uint64_t key) {
    pthread_mutex_lock(&g_games_mutex);

    int idx = find_game(key);
    if (idx >= 0) {
        ce_free(g_games[idx].engine);
        g_games[idx].active = false;
    }

    pthread_mutex_unlock(&g_games_mutex);
}

static GameState* get_game(uint64_t key) {
    pthread_mutex_lock(&g_games_mutex);
    int idx = find_game(key);
    pthread_mutex_unlock(&g_games_mutex);
    return idx >= 0 ? &g_games[idx] : NULL;
}

#ifndef MSG_EPHEMERAL
#  define MSG_EPHEMERAL 64
#endif

static void reply_ephemeral(struct discord *client,
                            const struct discord_interaction *event,
                            const char *content) {
    struct discord_interaction_response resp = {
        .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
        .data = &(struct discord_interaction_callback_data){
            .content = (char *)content,
            .flags   = MSG_EPHEMERAL,
        },
    };
    discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
}

static void reply_public(struct discord *client,
                        const struct discord_interaction *event,
                        const char *content) {
    struct discord_interaction_response resp = {
        .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
        .data = &(struct discord_interaction_callback_data){
            .content = (char *)content,
        },
    };
    discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
}

/* ---------------------------------------------------------------------------
 * Command handlers
 * --------------------------------------------------------------------------- */

static void on_chess_play(struct discord *client,
                          const struct discord_interaction *event) {
    if (!event->channel_id || !event->member || !event->member->user)
        return;

    uint64_t key = game_key(event->channel_id, event->member->user->id);
    GameState* game = get_game(key);

    if (game) {
        /* Game already in progress */
        reply_ephemeral(client, event,
            "You already have a game in progress in this channel! "
            "Use `/chess resign` to end it first.");
        return;
    }

    /* Create new game */
    game = create_game(key);
    if (!game) {
        reply_ephemeral(client, event,
            "Failed to create game. Too many games active.");
        return;
    }

    /* Randomly choose which side player is */
    int human_side = (rand() % 2 == 0) ? CE_WHITE : CE_BLACK;
    game->human_side = human_side;
    ce_new_game(game->engine, human_side);

    /* Prepare response */
    char board_str[512];
    snprintf(board_str, sizeof(board_str),
            "Chess game started! You are playing as **%s**.\n\n"
            "Use `/chess move <uci>` to make a move (e.g., `e2e4`).\n"
            "Use `/chess resign` to give up.\n\n"
            "Moves in UCI notation: source and destination squares (e.g., e2e4, g8f6).",
            human_side == CE_WHITE ? "White" : "Black");

    reply_public(client, event, board_str);

    /* If engine plays first, make its move */
    if (human_side == CE_BLACK) {
        char reply[8];
        if (ce_engine_move(game->engine, reply, sizeof(reply)) == CE_OK) {
            char follow_up[256];
            snprintf(follow_up, sizeof(follow_up),
                    "Engine plays: **%s**\nYour turn!", reply);
            reply_public(client, event, follow_up);
        }
    }
}

static void on_chess_move(struct discord *client,
                          const struct discord_interaction *event) {
    if (!event->channel_id || !event->member || !event->member->user ||
        !event->data || !event->data->options)
        return;

    uint64_t key = game_key(event->channel_id, event->member->user->id);
    GameState* game = get_game(key);

    if (!game) {
        reply_ephemeral(client, event,
            "No game in progress. Use `/chess play` to start one.");
        return;
    }

    /* Extract move string from options */
    const char* move_str = NULL;
    for (int i = 0; event->data->options[i]; i++) {
        if (strcmp(event->data->options[i]->name, "move") == 0) {
            move_str = event->data->options[i]->value;
            break;
        }
    }

    if (!move_str || move_str[0] == '\0') {
        reply_ephemeral(client, event, "Move string is required.");
        return;
    }

    /* Apply human move */
    int r = ce_apply_human_move(game->engine, move_str);

    if (r == CE_ILLEGAL_MOVE) {
        char explanation[1024];
        ce_explain_illegal_move(game->engine, move_str, explanation, sizeof(explanation));
        
        char response_text[1536];
        snprintf(response_text, sizeof(response_text),
                "**Illegal move:** `%s`\n\n```\n%s\n```", move_str, explanation);

        reply_ephemeral(client, event, response_text);
        return;
    }

    if (r == CE_GAME_OVER) {
        int result = ce_game_result(game->engine);
        const char* result_text = "Draw";
        if (result == 1) result_text = "White wins!";
        else if (result == 2) result_text = "Black wins!";

        char response_text[256];
        snprintf(response_text, sizeof(response_text),
                "Game over! %s", result_text);

        reply_public(client, event, response_text);
        delete_game(key);
        return;
    }

    /* Engine plays */
    char reply[8];
    int er = ce_engine_move(game->engine, reply, sizeof(reply));

    if (er == CE_GAME_OVER) {
        int result = ce_game_result(game->engine);
        const char* result_text = "Draw";
        if (result == 1) result_text = "White wins!";
        else if (result == 2) result_text = "Black wins!";

        char response_text[256];
        snprintf(response_text, sizeof(response_text),
                "Game over! %s", result_text);

        reply_public(client, event, response_text);
        delete_game(key);
        return;
    }

    char response_text[256];
    snprintf(response_text, sizeof(response_text),
            "You played: `%s`\nEngine plays: `%s`\n\nYour move!", move_str, reply);

    reply_public(client, event, response_text);
}

static void on_chess_resign(struct discord *client,
                            const struct discord_interaction *event) {
    if (!event->channel_id || !event->member || !event->member->user)
        return;

    uint64_t key = game_key(event->channel_id, event->member->user->id);
    GameState* game = get_game(key);

    if (!game) {
        reply_ephemeral(client, event, "No game in progress.");
        return;
    }

    delete_game(key);
    reply_public(client, event, "You have resigned from the game.");
}

/* ---------------------------------------------------------------------------
 * Slash command registration and initialization
 * --------------------------------------------------------------------------- */

void register_chess_commands(struct discord *client,
                            u64_snowflake_t application_id,
                            u64_snowflake_t guild_id) {
    /* Define subcommands */
    static struct discord_application_command_option play_opt = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_SUB_COMMAND,
        .name        = "play",
        .description = "Start a new game",
    };

    static struct discord_application_command_option move_str_opt = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name        = "move",
        .description = "Move in UCI notation (e.g., e2e4)",
        .required    = true,
    };
    static struct discord_application_command_option *move_opts[] = {
        &move_str_opt, NULL,
    };
    static struct discord_application_command_option move_opt = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_SUB_COMMAND,
        .name        = "move",
        .description = "Make a move",
        .options     = move_opts,
    };

    static struct discord_application_command_option resign_opt = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_SUB_COMMAND,
        .name        = "resign",
        .description = "Give up your game",
    };

    static struct discord_application_command_option *chess_opts[] = {
        &play_opt, &move_opt, &resign_opt, NULL,
    };

    static struct discord_create_guild_application_command_params p = {
        .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name        = "chess",
        .description = "Play chess against the engine",
        .options     = chess_opts,
    };

    discord_create_guild_application_command(client, application_id,
                                            guild_id, &p, NULL);
}

void chess_module_init(struct discord *client) {
    g_client = client;
    memset(g_games, 0, sizeof(g_games));
    g_game_count = 0;
    srand((unsigned)time(NULL));
}

void on_chess_interaction(struct discord *client,
                         const struct discord_interaction *event) {
    if (!event->data || !event->data->name)
        return;

    if (strcmp(event->data->name, "chess") != 0)
        return;

    if (!event->data->options)
        return;

    const char* subcommand = event->data->options[0]->name;

    if (strcmp(subcommand, "play") == 0) {
        on_chess_play(client, event);
    } else if (strcmp(subcommand, "move") == 0) {
        on_chess_move(client, event);
    } else if (strcmp(subcommand, "resign") == 0) {
        on_chess_resign(client, event);
    }
}
