#pragma once
/*  chess.h — Board representation, move encoding, and core declarations.
    All chess logic is C-compatible for easy embedding.               */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Piece constants                                                     */
/* ------------------------------------------------------------------ */
#define PIECE_NONE   0
#define PIECE_PAWN   1
#define PIECE_KNIGHT 2
#define PIECE_BISHOP 3
#define PIECE_ROOK   4
#define PIECE_QUEEN  5
#define PIECE_KING   6

#define COLOR_WHITE 0
#define COLOR_BLACK 1

/*  Piece encoding: bits[2:0] = type, bit[3] = color                  */
#define MAKE_PIECE(type, color) ((uint8_t)((type) | ((color) << 3)))
#define PIECE_TYPE(p)           ((int)((p) & 7))
#define PIECE_COLOR(p)          ((int)(((p) >> 3) & 1))

/* ------------------------------------------------------------------ */
/*  Squares: a1 = 0, h1 = 7, a8 = 56, h8 = 63                        */
/* ------------------------------------------------------------------ */
#define SQ(f, r)    ((r) * 8 + (f))
#define FILE_OF(sq) ((sq) & 7)
#define RANK_OF(sq) ((sq) >> 3)

/* ------------------------------------------------------------------ */
/*  Castling rights bits                                               */
/* ------------------------------------------------------------------ */
#define CR_WK 1
#define CR_WQ 2
#define CR_BK 4
#define CR_BQ 8

/* ------------------------------------------------------------------ */
/*  Move encoding (32-bit)                                             */
/*  bits[ 5: 0] = from square                                         */
/*  bits[11: 6] = to square                                           */
/*  bits[14:12] = promotion piece type                                */
/*  bits[17:15] = flags                                               */
/* ------------------------------------------------------------------ */
#define MF_NORMAL 0
#define MF_CASTLE 1
#define MF_EP     2
#define MF_PROMO  4

typedef uint32_t Move;
#define MOVE_NONE 0u

static inline Move make_move(int from, int to, int promo, int flags) {
    return (Move)((unsigned)from | ((unsigned)to << 6) |
                  ((unsigned)promo << 12) | ((unsigned)flags << 15));
}
#define MOVE_FROM(m)  ((int)((m) & 63u))
#define MOVE_TO(m)    ((int)(((m) >> 6) & 63u))
#define MOVE_PROMO(m) ((int)(((m) >> 12) & 7u))
#define MOVE_FLAGS(m) ((int)(((m) >> 15) & 7u))
#define IS_PROMO(m)   (MOVE_FLAGS(m) & MF_PROMO)
#define IS_CASTLE(m)  (MOVE_FLAGS(m) & MF_CASTLE)
#define IS_EP(m)      (MOVE_FLAGS(m) & MF_EP)

/* ------------------------------------------------------------------ */
/*  Move list                                                          */
/* ------------------------------------------------------------------ */
#define MAX_MOVES 256

typedef struct {
    Move moves[MAX_MOVES];
    int  count;
} MoveList;

/* ------------------------------------------------------------------ */
/*  Board state                                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t  board[64];   /* piece on each square, 0 = empty         */
    int      stm;         /* side to move: COLOR_WHITE / COLOR_BLACK  */
    int      castling;    /* castling rights bitmask                  */
    int      ep_sq;       /* en passant target square, -1 = none      */
    int      halfmove;    /* half-move clock (50-move rule)           */
    int      fullmove;
    uint64_t hash;        /* Zobrist hash                             */
} Board;

/* Saved state for unmake_move */
typedef struct {
    uint8_t  captured;
    int      castling;
    int      ep_sq;
    int      halfmove;
    uint64_t hash;
} Undo;

/* ------------------------------------------------------------------ */
/*  Game result codes                                                  */
/* ------------------------------------------------------------------ */
#define GAME_ONGOING   0
#define GAME_WHITE_WIN 1
#define GAME_BLACK_WIN 2
#define GAME_DRAW      3

/* ------------------------------------------------------------------ */
/*  NNUE feature size                                                  */
/* ------------------------------------------------------------------ */
#define NNUE_FEATURES 768  /* 12 piece-types × 64 squares             */

/* ------------------------------------------------------------------ */
/*  API                                                                */
/* ------------------------------------------------------------------ */
void  board_init(Board* b);
void  board_set_fen(Board* b, const char* fen);
void  board_get_fen(const Board* b, char* out, int size);
void  board_print(const Board* b);
void  board_make_move(Board* b, Move m, Undo* u);
void  board_unmake_move(Board* b, Move m, const Undo* u);
void  board_gen_moves(const Board* b, MoveList* ml);
void  board_gen_legal(Board* b, MoveList* ml);
bool  board_in_check(const Board* b, int color);
int   board_game_result(Board* b);
Move  board_parse_move(const Board* b, const char* str);
void  board_move_str(Move m, char* out);    /* writes UCI string      */
void  board_get_features(const Board* b, float* feat); /* 768 floats */
int   board_piece_idx(uint8_t piece);       /* 0-11, for Zobrist/feat */

#ifdef __cplusplus
}
#endif
