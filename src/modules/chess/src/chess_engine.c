/*  chess_engine.c — Public ChessEngine API implementation.           */
#include "chess_engine.h"
#include "chess.h"
#include "move_lawyer.h"
#include "nnue.h"
#include "search.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ================================================================== */
/*  Engine handle                                                      */
/* ================================================================== */
struct ChessEngine {
    NNUEWeights*    weights;
    SearchContext*  search;
    Board           board;
    int             human_side;
    int             search_depth;
    bool            training_active;
    uint64_t        rng_seed;
};

/* ================================================================== */
/*  Lifecycle                                                          */
/* ================================================================== */
ChessEngine* ce_create(const char* weights_path) {
    ChessEngine* e = (ChessEngine*)calloc(1, sizeof(ChessEngine));
    if (!e) return NULL;

    e->weights = nnue_alloc_weights();
    if (!e->weights) { free(e); return NULL; }

    e->rng_seed = 0xDEADBEEFCAFEull;

    if (weights_path && *weights_path) {
        if (!nnue_load(e->weights, weights_path)) {
            fprintf(stderr, "[Engine] Could not load '%s'. Using random weights.\n",
                    weights_path);
            nnue_init_random(e->weights, e->rng_seed);
        } else {
            printf("[Engine] Loaded weights from '%s'.\n", weights_path);
        }
    } else {
        nnue_init_random(e->weights, e->rng_seed);
        printf("[Engine] Initialised with random weights.\n");
    }

    e->search = search_create(e->weights);
    if (!e->search) { nnue_free_weights(e->weights); free(e); return NULL; }

    e->search_depth = 6;
    e->human_side   = CE_WHITE;
    e->training_active = false;

    board_init(&e->board);
    return e;
}

void ce_free(ChessEngine* e) {
    if (!e) return;
    search_free(e->search);
    nnue_free_weights(e->weights);
    free(e);
}

bool ce_save_weights(const ChessEngine* e, const char* path) {
    return nnue_save(e->weights, path);
}

bool ce_load_weights(ChessEngine* e, const char* path) {
    if (!nnue_load(e->weights, path)) return false;
    /* weights pointer is shared with search, so it updates automatically */
    return true;
}

/* ================================================================== */
/*  Gameplay                                                           */
/* ================================================================== */
void ce_new_game(ChessEngine* e, int human_side) {
    board_init(&e->board);
    e->human_side = human_side;
    search_clear_tt(e->search);
}

void ce_set_fen(ChessEngine* e, const char* fen) {
    printf("Set FEN to: %s", fen);
    board_set_fen(&e->board, fen);
}

void ce_get_fen(const ChessEngine* e, char* out, int size) {
    board_get_fen(&e->board, out, size);
}

void ce_set_depth(ChessEngine* e, int depth) {
    if (depth >= 1 && depth <= SEARCH_MAX_DEPTH)
        e->search_depth = depth;
}

int ce_game_result(ChessEngine* e) {
    return board_game_result(&e->board);
}

void ce_print_board(const ChessEngine* e) {
    board_print(&e->board);
}

void ce_print_eval(ChessEngine* e) {
    float feat[NNUE_FEATURES];
    board_get_features(&e->board, feat);
    nnue_refresh(&e->search->nnue, feat);
    int ev = search_evaluate(&e->search->nnue, e->board.stm);
    /* always print from White's perspective */
    if (e->board.stm == COLOR_BLACK) ev = -ev;
    printf("[Engine] Evaluation: %+d cp (%s)\n", ev,
           ev > 0 ? "White advantage" :
           ev < 0 ? "Black advantage" : "Equal");
}

int ce_apply_human_move(ChessEngine* e, const char* move_str) {
    int result = board_game_result(&e->board);
    if (result != GAME_ONGOING) return CE_GAME_OVER;

    /* Strip leading/trailing whitespace */
    while (*move_str == ' ' || *move_str == '\n' || *move_str == '\r')
        move_str++;

    Move m = board_parse_move(&e->board, move_str);
    if (m == MOVE_NONE) return CE_ILLEGAL_MOVE;

    Undo u;
    board_make_move(&e->board, m, &u);
    return CE_OK;
}

int ce_engine_move(ChessEngine* e, char* move_out, int move_out_size) {
    int result = board_game_result(&e->board);
    if (result != GAME_ONGOING) return CE_GAME_OVER;

    search_set_position(e->search, &e->board);

    /* Ensure NNUE is synced */
    float feat[NNUE_FEATURES];
    board_get_features(&e->board, feat);
    nnue_refresh(&e->search->nnue, feat);

    SearchResult sr = search_run(e->search, e->search_depth);

    if (sr.best_move == MOVE_NONE) return CE_GAME_OVER;

    char uci[8];
    board_move_str(sr.best_move, uci);
    snprintf(move_out, (size_t)move_out_size, "%s", uci);

    Undo u;
    board_make_move(&e->board, sr.best_move, &u);

    printf("[Engine] Move: %s  (score: %+d cp, depth: %d, nodes: %llu)\n",
           uci, sr.score, sr.depth,
           (unsigned long long)sr.nodes);

    return CE_OK;
}

void ce_explain_illegal_move(const ChessEngine* e, const char* move_str,
                             char* out, int out_size) {
    move_lawyer_explain(&e->board, move_str, out, out_size);
}