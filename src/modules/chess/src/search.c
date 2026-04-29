/*  search.c — Iterative-deepening alpha-beta with NNUE leaf eval.   */
#include "search.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

/* ================================================================== */
/*  Piece-square tables for move ordering (material + PST delta)     */
/* ================================================================== */
static const int PIECE_VALUE[7] = {0, 100, 320, 330, 500, 900, 20000};

/* Simplified MVV-LVA table [victim][attacker] */
static const int MVV_LVA[7][7] = {
    {0,  0,  0,  0,  0,  0,  0},
    {0, 105,104,103,102,101,100},
    {0, 205,204,203,202,201,200},
    {0, 305,304,303,302,301,300},
    {0, 405,404,403,402,401,400},
    {0, 505,504,503,502,501,500},
    {0, 605,604,603,602,601,600},
};

/* ================================================================== */
/*  Score moves for ordering                                           */
/* ================================================================== */
static int score_move(const Board* b, Move m, Move tt_move,
                       const Move killers[2], const int history[64][64]) {
    if (m == tt_move) return 2000000;

    int from = MOVE_FROM(m), to = MOVE_TO(m);
    uint8_t victim = b->board[to];

    if (IS_PROMO(m)) return 1500000 + PIECE_VALUE[MOVE_PROMO(m)];

    if (victim) {
        uint8_t attacker = b->board[from];
        return 1000000 +
               MVV_LVA[PIECE_TYPE(victim)][PIECE_TYPE(attacker)];
    }

    if (m == killers[0]) return 900000;
    if (m == killers[1]) return 890000;

    return history[from][to];
}

static void sort_moves(MoveList* ml, int start, Board* b, Move tt_move,
                        const Move killers[2], const int hist[64][64]) {
    /* partial insertion sort from 'start' */
    for (int i = start + 1; i < ml->count; i++) {
        Move m = ml->moves[i];
        int s = score_move(b, m, tt_move, killers, hist);
        int j = i - 1;
        while (j >= start &&
               score_move(b, ml->moves[j], tt_move, killers, hist) < s) {
            ml->moves[j+1] = ml->moves[j];
            j--;
        }
        ml->moves[j+1] = m;
    }
}

/* ================================================================== */
/*  TT operations                                                      */
/* ================================================================== */
static TTEntry* tt_probe(SearchContext* ctx, uint64_t key) {
    TTEntry* e = &ctx->tt[key & (TT_SIZE - 1)];
    return (e->key == key) ? e : NULL;
}

static void tt_store(SearchContext* ctx, uint64_t key, Move best,
                      int score, int depth, int flag) {
    TTEntry* e = &ctx->tt[key & (TT_SIZE - 1)];
    /* always replace if depth >= existing */
    if (e->key != key || depth >= e->depth) {
        e->key   = key;
        e->best  = best;
        e->score = score;
        e->depth = (int8_t)depth;
        e->flag  = (uint8_t)flag;
    }
}

/* ================================================================== */
/*  NNUE helpers                                                       */
/* ================================================================== */
int search_evaluate(NNUEContext* nc, int stm) {
    return (int)nnue_evaluate(nc, stm);
}

/* ================================================================== */
/*  Quiescence search                                                  */
/* ================================================================== */
int search_quiesce(SearchContext* ctx, Board* b,
                   NNUEContext* nc, int alpha, int beta) {
    ctx->nodes++;

    int stand_pat = search_evaluate(nc, b->stm);
    if (stand_pat >= beta) return beta;
    if (stand_pat > alpha) alpha = stand_pat;

    /* generate capture moves only */
    MoveList ml;
    board_gen_legal(b, &ml);

    for (int i = 0; i < ml.count; i++) {
        Move m = ml.moves[i];
        if (!b->board[MOVE_TO(m)] && !IS_EP(m) && !IS_PROMO(m)) continue;

        Undo u;
        float feat[NNUE_FEATURES];
        board_make_move(b, m, &u);
        board_get_features(b, feat);
        NNUEContext nc2 = *nc;
        nc2.weights = nc->weights;
        nnue_refresh(&nc2, feat);

        int score = -search_quiesce(ctx, b, &nc2, -beta, -alpha);
        board_unmake_move(b, m, &u);

        if (ctx->stop) return 0;
        if (score >= beta) return beta;
        if (score > alpha) alpha = score;
    }
    return alpha;
}

/* ================================================================== */
/*  Accumulator helpers — add near the top of search.c                */
/*  (or move into nnue.c and expose via nnue.h)                       */
/* ================================================================== */

/*  Map a piece + square to its weight-matrix row index.
 *  This MUST match the ordering used by board_get_features() /
 *  nnue_refresh().  Adjust PIECE_COLOR / PIECE_TYPE shift if your
 *  piece encoding differs.                                            */
static inline int feat_white(uint8_t piece, int sq) {
    int blk = PIECE_COLOR(piece) * 6 + (PIECE_TYPE(piece) - 1);
    return blk * 64 + sq;
}

/*  Black accumulator mirrors the board rank-wise (sq ^ 56).          */
static inline int feat_black(uint8_t piece, int sq) {
    int blk = PIECE_COLOR(piece) * 6 + (PIECE_TYPE(piece) - 1);
    return blk * 64 + (sq ^ 56);
}

/*  Update both accumulator perspectives for one piece on one square. */
static void acc_update(NNUEContext* nc, uint8_t piece, int sq, bool add) {
    nnue_update_feature(nc, feat_white(piece, sq), COLOR_WHITE, add);
    nnue_update_feature(nc, feat_black(piece, sq), COLOR_BLACK, add);
}

/*  Apply the incremental NNUE delta for move m.
 *
 *  All parameters that describe the board come from a snapshot taken
 *  BEFORE board_make_move(), because the board has changed by the
 *  time we want to update the accumulator.
 *
 *  moved       — piece that left 'from'
 *  target      — piece previously on 'to' (0 if empty; 0 for EP too)
 *  ep_victim   — en-passant captured pawn (0 if not EP)
 *  ep_sq       — square the EP pawn sat on (meaningful only if ep_victim != 0)
 *  rook        — rook involved in castling (0 if not castling)
 *  rook_from / rook_to — rook squares (meaningful only if rook != 0)
 */
static void acc_apply_move(NNUEContext* nc,
                            uint8_t moved,   int from,  int to,
                            uint8_t target,
                            uint8_t ep_victim, int ep_sq,
                            uint8_t rook, int rook_from, int rook_to,
                            Move m) {
    /* 1. Lift the moving piece off its source square. */
    acc_update(nc, moved, from, false);

    /* 2. Remove any directly-captured piece. */
    if (target)
        acc_update(nc, target, to, false);

    /* 3. Handle special move types. */
    if (IS_EP(m)) {
        /* En-passant: captured pawn is NOT on 'to'. */
        acc_update(nc, ep_victim, ep_sq, false);
        acc_update(nc, moved,     to,    true);
    } else if (IS_PROMO(m)) {
        /* Replace the pawn with the promoted piece. */
        uint8_t promo = MAKE_PIECE(PIECE_COLOR(moved), MOVE_PROMO(m));
        acc_update(nc, promo, to, true);
    } else {
        /* Quiet move or normal capture: land on destination. */
        acc_update(nc, moved, to, true);

        /* Castling: the rook also moves. */
        if (rook) {
            acc_update(nc, rook, rook_from, false);
            acc_update(nc, rook, rook_to,   true);
        }
    }
}

/* ================================================================== */
/*  Alpha-beta negamax — rewritten with fix #1 and fix #2             */
/* ================================================================== */
static int alphabeta(SearchContext* ctx, Board* b, NNUEContext* nc,
                      int depth, int alpha, int beta, int ply) {
    if (ctx->stop) return 0;
    ctx->nodes++;

    /* Mate distance pruning */
    int alpha_orig = alpha;
    if (alpha < -SEARCH_MATE_SCORE + ply) alpha = -SEARCH_MATE_SCORE + ply;
    if (beta  >  SEARCH_MATE_SCORE - ply) beta  =  SEARCH_MATE_SCORE - ply;
    if (alpha >= beta) return alpha;

    /* TT lookup */
    TTEntry* tt = tt_probe(ctx, b->hash);
    Move tt_move = MOVE_NONE;
    if (tt) {
        tt_move = tt->best;
        if (tt->depth >= depth) {
            int s = tt->score;
            if (tt->flag == TT_EXACT) return s;
            if (tt->flag == TT_LOWER && s > alpha) alpha = s;
            if (tt->flag == TT_UPPER && s < beta)  beta  = s;
            if (alpha >= beta) return s;
        }
    }

    /* Terminal checks */
    int result = board_game_result(b);
    if (result != GAME_ONGOING) {
        if (result == GAME_DRAW) return 0;
        return -SEARCH_MATE_SCORE + ply;
    }

    if (depth <= 0)
        return search_quiesce(ctx, b, nc, alpha, beta);

    /* Null-move pruning */
    if (depth >= 3 && !board_in_check(b, b->stm)) {
        Board nb = *b;
        NNUEContext nc_null = *nc;          /* accumulator is still valid: */
        nb.ep_sq  = -1;                    /* no pieces moved, just stm flip */
        nb.stm   ^= 1;
        /* Note: nb.hash is not updated for null moves; TT usage here is approximate */
        int R = (depth >= 6) ? 3 : 2;
        int null_score = -alphabeta(ctx, &nb, &nc_null,
                                     depth - 1 - R, -beta, -beta + 1, ply + 1);
        if (null_score >= beta) return beta;
    }

    MoveList ml;
    board_gen_legal(b, &ml);
    sort_moves(&ml, 0, b, tt_move, ctx->killers[ply], ctx->history);

    Move best_move  = MOVE_NONE;
    int  best_score = -SEARCH_INF;

    for (int i = 0; i < ml.count; i++) {
        Move m    = ml.moves[i];
        int  from = MOVE_FROM(m);
        int  to   = MOVE_TO(m);

        /* ---------------------------------------------------------- */
        /*  FIX #1: snapshot everything we need from the pre-move     */
        /*  board.  After board_make_move() this information is gone.  */
        /* ---------------------------------------------------------- */
        uint8_t moved  = b->board[from];
        uint8_t target = b->board[to];  /* 0 for quiet moves and EP */

        /*  is_quiet must be determined NOW, before make_move,        */
        /*  because board[to] will hold the moved piece afterward.    */
        int is_quiet = !target && !IS_EP(m) && !IS_PROMO(m);

        /*  Castling delta: capture rook info before it moves.        */
        uint8_t rook      = 0;
        int     rook_from = 0, rook_to = 0;
        if (PIECE_TYPE(moved) == PIECE_KING && abs(to - from) == 2) {
            rook_from = (to > from) ? from + 3 : from - 4;
            rook_to   = (to > from) ? from + 1 : from - 1;
            rook      = b->board[rook_from];
        }

        /*  En-passant delta: captured pawn is off 'to'.              */
        int ep_sq = IS_EP(m)
                    ? to + (b->stm == COLOR_WHITE ? -8 : 8)
                    : 0;

        /* ---------------------------------------------------------- */
        /*  Make move, then update accumulator incrementally.         */
        /* ---------------------------------------------------------- */
        Undo u;
        board_make_move(b, m, &u);

        /*  FIX #2: copy the accumulator (O(L1)), then apply only     */
        /*  the feature deltas that changed (O(features_changed×L1)). */
        /*  Previously this was board_get_features + nnue_refresh,    */
        /*  which is O(NNUE_INPUT×L1) — ~100× more work per node.    */
        NNUEContext nc2 = *nc;
        acc_apply_move(&nc2,
                        moved,  from,   to,
                        target,
                        u.captured, ep_sq,      /* u.captured is EP pawn too */
                        rook,   rook_from, rook_to,
                        m);

        /* ---------------------------------------------------------- */
        /*  Recursive search                                           */
        /* ---------------------------------------------------------- */
        int score;
        if (i == 0) {
            score = -alphabeta(ctx, b, &nc2, depth - 1,
                                -beta, -alpha, ply + 1);
        } else {
            int lmr_depth = depth - 1;

            /*  FIX #1 (cont): use the pre-captured is_quiet flag.    */
            /*  FIX #1 (cont): fix operator precedence in reduction — */
            /*  was (int)sqrt(i)/2 = 0 for i<4, now (int)(sqrt(i)/2) */
            if (i >= 4 && depth >= 3 && is_quiet
                       && !board_in_check(b, b->stm)) {
                lmr_depth = depth - 1 - (int)(sqrt((double)i) / 2);
                if (lmr_depth < 1) lmr_depth = 1;
            }

            score = -alphabeta(ctx, b, &nc2, lmr_depth,
                                -alpha - 1, -alpha, ply + 1);
            if (score > alpha && lmr_depth < depth - 1)
                score = -alphabeta(ctx, b, &nc2, depth - 1,
                                    -beta, -alpha, ply + 1);
        }

        board_unmake_move(b, m, &u);
        if (ctx->stop) return 0;

        if (score > best_score) {
            best_score = score;
            best_move  = m;
        }
        if (score > alpha) {
            alpha = score;
            /*  FIX #1: guard with is_quiet captured before make_move. */
            if (is_quiet)
                ctx->history[from][to] += depth * depth;
        }
        if (alpha >= beta) {
            /*  FIX #1: guard with is_quiet captured before make_move. */
            if (is_quiet) {
                ctx->killers[ply][1] = ctx->killers[ply][0];
                ctx->killers[ply][0] = m;
            }
            tt_store(ctx, b->hash, m, beta, depth, TT_LOWER);
            return beta;
        }
    }

    int flag = (best_score <= alpha_orig) ? TT_UPPER : TT_EXACT;
    tt_store(ctx, b->hash, best_move, best_score, depth, flag);
    return best_score;
}

/* ================================================================== */
/*  Public API                                                         */
/* ================================================================== */
SearchContext* search_create(NNUEWeights* weights) {
    SearchContext* ctx = (SearchContext*)calloc(1, sizeof(SearchContext));
    if (!ctx) return NULL;
    ctx->tt = (TTEntry*)calloc(TT_SIZE, sizeof(TTEntry));
    if (!ctx->tt) { free(ctx); return NULL; }
    ctx->nnue.weights = weights;
    return ctx;
}

void search_free(SearchContext* ctx) {
    if (!ctx) return;
    free(ctx->tt);
    free(ctx);
}

void search_clear_tt(SearchContext* ctx) {
    memset(ctx->tt, 0, TT_SIZE * sizeof(TTEntry));
}

void search_set_position(SearchContext* ctx, const Board* b) {
    ctx->root = *b;
}

SearchResult search_run(SearchContext* ctx, int depth) {
    SearchResult res = {MOVE_NONE, 0, 0, 0};
    ctx->nodes = 0;
    ctx->stop  = 0;
    memset(ctx->killers, 0, sizeof(ctx->killers));
    memset(ctx->history, 0, sizeof(ctx->history));

    /* Refresh NNUE from root position */
    float feat[NNUE_FEATURES];
    board_get_features(&ctx->root, feat);
    nnue_refresh(&ctx->nnue, feat);

    Board b = ctx->root;

    for (int d = 1; d <= depth; d++) {
        ctx->nodes = 0;
        NNUEContext nc = ctx->nnue;
        nc.weights = ctx->nnue.weights;

        int score = alphabeta(ctx, &b, &nc, d,
                               -SEARCH_INF, SEARCH_INF, 0);
        if (ctx->stop) break;

        /* best move from TT at root */
        TTEntry* tt = tt_probe(ctx, b.hash);
        if (tt && tt->best != MOVE_NONE) {
            res.best_move = tt->best;
        }
        res.score = score;
        res.depth = d;
        res.nodes = ctx->nodes;
    }
    return res;
}