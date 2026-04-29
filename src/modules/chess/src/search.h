#pragma once
/*  search.h — Minimax alpha-beta search with iterative deepening.   */

#include "chess.h"
#include "nnue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Search limits                                                      */
/* ------------------------------------------------------------------ */
#define SEARCH_MAX_DEPTH  32
#define SEARCH_INF        1000000
#define SEARCH_MATE_SCORE 900000
#define SEARCH_MATE_IN(n) (SEARCH_MATE_SCORE - (n))
#define IS_MATE_SCORE(s)  (abs(s) >= (SEARCH_MATE_SCORE - SEARCH_MAX_DEPTH))

/* ------------------------------------------------------------------ */
/*  Transposition table entry                                          */
/* ------------------------------------------------------------------ */
#define TT_EXACT 0
#define TT_LOWER 1   /* alpha (lower bound)  */
#define TT_UPPER 2   /* beta  (upper bound)  */

typedef struct {
    uint64_t key;
    Move     best;
    int      score;
    int8_t   depth;
    uint8_t  flag;
} TTEntry;

#define TT_SIZE (1 << 22)   /* 4M entries, ~96 MB                    */

/* ------------------------------------------------------------------ */
/*  Search context                                                      */
/* ------------------------------------------------------------------ */
typedef struct {
    NNUEContext  nnue;         /* owns a NNUEContext with weights ptr */
    TTEntry*     tt;           /* transposition table                 */
    Board        root;         /* root board (copy)                   */
    uint64_t     nodes;
    int          max_depth;
    int          time_ms;      /* soft time limit, 0 = depth only     */
    volatile int stop;         /* set to 1 to abort                   */

    /* killer moves: [depth][2]                                       */
    Move killers[SEARCH_MAX_DEPTH][2];

    /* history heuristic [from][to]                                   */
    int  history[64][64];
} SearchContext;

/* ------------------------------------------------------------------ */
/*  Search info returned per search call                               */
/* ------------------------------------------------------------------ */
typedef struct {
    Move  best_move;
    int   score;       /* centipawns, white-relative                  */
    int   depth;
    uint64_t nodes;
} SearchResult;

/* ------------------------------------------------------------------ */
/*  API                                                                */
/* ------------------------------------------------------------------ */
SearchContext* search_create(NNUEWeights* weights);
void           search_free(SearchContext* ctx);
void           search_clear_tt(SearchContext* ctx);
void           search_set_position(SearchContext* ctx, const Board* b);

/*  Run search to given depth.  Returns best move + score.            */
SearchResult   search_run(SearchContext* ctx, int depth);

/*  Quiescence search (captures only), callable standalone for eval.  */
int            search_quiesce(SearchContext* ctx, Board* b,
                              NNUEContext* nc, int alpha, int beta);

/*  Evaluate leaf using NNUE accumulator.                             */
int            search_evaluate(NNUEContext* nc, int stm);

#ifdef __cplusplus
}
#endif
