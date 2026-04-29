/*
 * chess.h
 *
 * Public interface for the chess module.
 *
 * Provides slash commands for playing chess on Discord:
 *   /chess play  — Start a new game vs the engine
 *   /chess move  — Make a move
 *   /chess board — Display the current board state
 *   /chess resign — Give up the current game
 *
 * Game state is stored per player (user + channel combination).
 */

#ifndef CHESS_H
#define CHESS_H

#include <orca/discord.h>

/* ---------------------------------------------------------------------------
 * chess_module_init
 *
 * Must be called once during bot startup with the fully-initialised
 * discord client.  The pointer is stored internally and used by every
 * subsequent chess interaction.
 * --------------------------------------------------------------------------- */
void chess_module_init(struct discord *client);

/* ---------------------------------------------------------------------------
 * register_chess_commands
 *
 * Register the chess slash commands for the given guild.
 * If guild_id == 0, commands are registered globally.
 * --------------------------------------------------------------------------- */
void register_chess_commands(struct discord *client,
                            u64_snowflake_t application_id,
                            u64_snowflake_t guild_id);

/* ---------------------------------------------------------------------------
 * on_chess_interaction
 *
 * Gateway event handler for chess command interactions.
 * Called from main's on_interaction_create_combined.
 * --------------------------------------------------------------------------- */
void on_chess_interaction(struct discord *client,
                         const struct discord_interaction *event);

#endif // CHESS_H
