#pragma once
/*  chess_engine.h — Single-header public API.
    Include only this file when embedding the engine.

    ── Quick start (gameplay) ──────────────────────────────────────
        ChessEngine* e = ce_create("model.bin");
        ce_new_game(e, CE_WHITE);              // human plays White
        ce_print_board(e);
        while (!ce_game_over(e)) {
            char move[8];
            fgets(move, sizeof move, stdin);
            if (!ce_apply_human_move(e, move)) { puts("Illegal"); continue; }
            ce_print_board(e);
            char reply[8];
            ce_engine_move(e, reply, sizeof reply);
            printf("Engine: %s\n", reply);
            ce_print_board(e);
        }
        ce_free(e);

    ── Quick start (training) ──────────────────────────────────────
        ChessEngine* e = ce_create(NULL);      // random init
        ce_train_start(e, "checkpoints/");
        sleep(3600);
        ce_train_stop(e);
        ce_save_weights(e, "model.bin");
        ce_free(e);
    ─────────────────────────────────────────────────────────────── */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Opaque engine handle                                               */
/* ------------------------------------------------------------------ */
typedef struct ChessEngine ChessEngine;

/* ------------------------------------------------------------------ */
/*  Side constants                                                     */
/* ------------------------------------------------------------------ */
#define CE_WHITE 0
#define CE_BLACK 1
#define CE_AUTO  2   /* engine picks the side it did NOT just play    */

/* ------------------------------------------------------------------ */
/*  Result codes                                                       */
/* ------------------------------------------------------------------ */
#define CE_OK            0
#define CE_ILLEGAL_MOVE -1
#define CE_GAME_OVER    -2
#define CE_ERROR        -3

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                          */
/* ------------------------------------------------------------------ */

/*  Create engine.  weights_path == NULL → random weight initialisation. */
ChessEngine* ce_create(const char* weights_path);
void         ce_free(ChessEngine* e);

/*  Save / load weights independently of full engine lifecycle.       */
bool ce_save_weights(const ChessEngine* e, const char* path);
bool ce_load_weights(ChessEngine* e, const char* path);

/* ------------------------------------------------------------------ */
/*  Normal gameplay mode                                               */
/* ------------------------------------------------------------------ */

/*  Start a new game.  human_side = CE_WHITE / CE_BLACK.              */
void ce_new_game(ChessEngine* e, int human_side);

/*  Set position from FEN string.                                     */
void ce_set_fen(ChessEngine* e, const char* fen);

/*  Get current position as FEN string.                               */
void ce_get_fen(const ChessEngine* e, char* out, int size);

/*  Apply a move made by the human player.
    move_str: UCI ("e2e4") or short algebraic ("e4", "Nf3", "O-O").
    Returns CE_OK on success, CE_ILLEGAL_MOVE if illegal.            */
int  ce_apply_human_move(ChessEngine* e, const char* move_str);

/*  Let the engine pick and apply its move.
    move_out: buffer for the UCI move string (at least 6 chars).
    Returns CE_OK, or CE_GAME_OVER if no legal moves.                */
int  ce_engine_move(ChessEngine* e, char* move_out, int move_out_size);

/*  Set search depth for gameplay mode (default 6).                   */
void ce_set_depth(ChessEngine* e, int depth);

/*  Is the game finished?  Returns GAME_ONGOING / WHITE_WIN / etc.    */
int  ce_game_result(ChessEngine* e);

/*  Print a pretty board to stdout with coordinates.                  */
void ce_print_board(const ChessEngine* e);

/*  Print engine evaluation for the current position (centipawns).   */
void ce_print_eval(ChessEngine* e);

/*  Explain why a move is illegal.
    Fills `out` with a bullet-pointed list of all reasons why move_str
    is invalid (or format error details).  `out` is always NUL-terminated. */
void ce_explain_illegal_move(const ChessEngine* e, const char* move_str,
                             char* out, int out_size);

/* ------------------------------------------------------------------ */
/*  Training mode                                                      */
/* ------------------------------------------------------------------ */

/*  Start background training.  checkpoint_dir is where .bin files
    will be written periodically.  Returns CE_OK or CE_ERROR.        */
int  ce_train_start(ChessEngine* e, const char* checkpoint_dir);

/*  Stop training and join all threads.                               */
void ce_train_stop(ChessEngine* e);

/*  Print a brief training status line.                               */
void ce_train_status(const ChessEngine* e);

/*  Poll: is training currently running?                              */
bool ce_train_running(const ChessEngine* e);

/* ------------------------------------------------------------------ */
/*  Utility                                                            */
/* ------------------------------------------------------------------ */

/*  Seed the internal Zobrist / weight RNG.                           */
void ce_set_seed(ChessEngine* e, uint64_t seed);

#ifdef __cplusplus
}
#endif
