/*  chess.c — Board representation, Zobrist, and move generation.     */
#include "chess.h"
#include <ctype.h>
#include <assert.h>

/* ================================================================== */
/*  Zobrist tables                                                     */
/* ================================================================== */
static uint64_t ZOB_PIECE[12][64];   /* piece-index × square         */
static uint64_t ZOB_CASTLE[16];
static uint64_t ZOB_EP[8];           /* ep file                      */
static uint64_t ZOB_STM;             /* flip when side-to-move changes*/
static int      zob_init_done = 0;

static uint64_t zob_rand(uint64_t* s) {
    *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17;
    return *s;
}

static void zob_init(void) {
    if (zob_init_done) return;
    uint64_t s = 0xDEADBEEFCAFEBABEull;
    for (int p = 0; p < 12; p++)
        for (int sq = 0; sq < 64; sq++)
            ZOB_PIECE[p][sq] = zob_rand(&s);
    for (int i = 0; i < 16; i++) ZOB_CASTLE[i] = zob_rand(&s);
    for (int i = 0;  i < 8;  i++) ZOB_EP[i]     = zob_rand(&s);
    ZOB_STM = zob_rand(&s);
    zob_init_done = 1;
}

int board_piece_idx(uint8_t piece) {
    if (!piece) return -1;
    /* white pieces 0-5, black pieces 6-11 */
    return (PIECE_COLOR(piece) * 6) + (PIECE_TYPE(piece) - 1);
}

static uint64_t board_compute_hash(const Board* b) {
    uint64_t h = 0;
    for (int sq = 0; sq < 64; sq++) {
        uint8_t p = b->board[sq];
        if (p) h ^= ZOB_PIECE[board_piece_idx(p)][sq];
    }
    h ^= ZOB_CASTLE[b->castling & 15];
    if (b->ep_sq >= 0) h ^= ZOB_EP[FILE_OF(b->ep_sq)];
    if (b->stm == COLOR_BLACK) h ^= ZOB_STM;
    return h;
}

/* ================================================================== */
/*  Board initialisation                                               */
/* ================================================================== */
void board_init(Board* b) {
    zob_init();
    board_set_fen(b, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
}

void board_set_fen(Board* b, const char* fen) {
    zob_init();
    memset(b->board, 0, 64);
    b->stm = COLOR_WHITE;
    b->castling = 0;
    b->ep_sq = -1;
    b->halfmove = 0;
    b->fullmove = 1;

    static const char* piece_chars = "PNBRQK";

    int rank = 7, file = 0;
    const char* p = fen;

    /* piece placement */
    while (*p && *p != ' ') {
        if (*p == '/') { rank--; file = 0; }
        else if (*p >= '1' && *p <= '8') { file += *p - '0'; }
        else {
            int color = islower((unsigned char)*p) ? COLOR_BLACK : COLOR_WHITE;
            char c = (char)toupper((unsigned char)*p);
            const char* found = strchr(piece_chars, c);
            if (found) {
                int type = (int)(found - piece_chars) + 1;
                b->board[SQ(file, rank)] = MAKE_PIECE(type, color);
                file++;
            }
        }
        p++;
    }
    if (*p == ' ') p++;

    /* side to move */
    b->stm = (*p == 'b') ? COLOR_BLACK : COLOR_WHITE;
    while (*p && *p != ' ') p++;
    if (*p == ' ') p++;

    /* castling */
    while (*p && *p != ' ') {
        if (*p == 'K') b->castling |= CR_WK;
        if (*p == 'Q') b->castling |= CR_WQ;
        if (*p == 'k') b->castling |= CR_BK;
        if (*p == 'q') b->castling |= CR_BQ;
        p++;
    }
    if (*p == ' ') p++;

    /* en passant */
    if (*p != '-') {
        int f = *p - 'a'; p++;
        int r = *p - '1'; p++;
        b->ep_sq = SQ(f, r);
    } else { p++; }
    if (*p == ' ') p++;

    /* clocks */
    b->halfmove = atoi(p);
    while (*p && *p != ' ') p++;
    if (*p == ' ') p++;
    b->fullmove = atoi(p);

    b->hash = board_compute_hash(b);
}

void board_get_fen(const Board* b, char* out, int size) {
    static const char piece_letters[] = ".pnbrqk.PNBRQK";
    char* p = out;
    for (int rank = 7; rank >= 0; rank--) {
        int empty = 0;
        for (int file = 0; file < 8; file++) {
            uint8_t pc = b->board[SQ(file, rank)];
            if (!pc) { empty++; continue; }
            if (empty) { *p++ = (char)('0' + empty); empty = 0; }
            int color = PIECE_COLOR(pc);
            int type  = PIECE_TYPE(pc);
            int idx   = color ? (type + 7) : type;
            *p++ = piece_letters[idx];
        }
        if (empty) *p++ = (char)('0' + empty);
        if (rank > 0) *p++ = '/';
    }
    *p++ = ' ';
    *p++ = (b->stm == COLOR_WHITE) ? 'w' : 'b';
    *p++ = ' ';
    if (!b->castling) { *p++ = '-'; }
    else {
        if (b->castling & CR_WK) *p++ = 'K';
        if (b->castling & CR_WQ) *p++ = 'Q';
        if (b->castling & CR_BK) *p++ = 'k';
        if (b->castling & CR_BQ) *p++ = 'q';
    }
    *p++ = ' ';
    if (b->ep_sq < 0) { *p++ = '-'; }
    else {
        *p++ = (char)('a' + FILE_OF(b->ep_sq));
        *p++ = (char)('1' + RANK_OF(b->ep_sq));
    }
    snprintf(p, (size_t)(size - (int)(p - out)),
             " %d %d", b->halfmove, b->fullmove);
}

/* ================================================================== */
/*  Board printing                                                     */
/* ================================================================== */
void board_print(const Board* b) {
    static const char* pc_unicode[] = {
        ".", "♙","♘","♗","♖","♕","♔",".",
            "♟","♞","♝","♜","♛","♚"
    };
    printf("\n  +---+---+---+---+---+---+---+---+\n");
    for (int rank = 7; rank >= 0; rank--) {
        printf("%d |", rank + 1);
        for (int file = 0; file < 8; file++) {
            uint8_t pc = b->board[SQ(file, rank)];
            int color = pc ? PIECE_COLOR(pc) : 0;
            int type  = pc ? PIECE_TYPE(pc)  : 0;
            int idx   = pc ? (color ? type : (type + 7)) : 0;
            printf(" %s |", pc_unicode[idx]);
        }
        printf("\n  +---+---+---+---+---+---+---+---+\n");
    }
    printf("    a   b   c   d   e   f   g   h\n");
    printf("  Side to move: %s\n\n",
           b->stm == COLOR_WHITE ? "White" : "Black");
}

/* ================================================================== */
/*  Attacked-square logic                                              */
/* ================================================================== */
static const int KNIGHT_OFFSETS[8][2] = {
    {-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}
};
static const int BISHOP_DIRS[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};
static const int ROOK_DIRS[4][2]   = {{-1,0},{1,0},{0,-1},{0,1}};
static const int KING_DIRS[8][2]   = {
    {-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}
};

bool board_in_check(const Board* b, int color) {
    /* find king */
    int king_sq = -1;
    uint8_t king_pc = MAKE_PIECE(PIECE_KING, color);
    for (int sq = 0; sq < 64; sq++) {
        if (b->board[sq] == king_pc) { king_sq = sq; break; }
    }
    if (king_sq < 0) return true;   /* shouldn't happen, safety      */

    int kf = FILE_OF(king_sq), kr = RANK_OF(king_sq);
    int opp = color ^ 1;

    /* pawns */
    int pawn_dir = (color == COLOR_WHITE) ? 1 : -1;
    for (int df = -1; df <= 1; df += 2) {
        int f = kf + df, r = kr + pawn_dir;
        if (f >= 0 && f < 8 && r >= 0 && r < 8) {
            uint8_t pc = b->board[SQ(f, r)];
            if (PIECE_TYPE(pc) == PIECE_PAWN && PIECE_COLOR(pc) == opp)
                return true;
        }
    }
    /* knights */
    for (int i = 0; i < 8; i++) {
        int f = kf + KNIGHT_OFFSETS[i][0];
        int r = kr + KNIGHT_OFFSETS[i][1];
        if (f < 0 || f > 7 || r < 0 || r > 7) continue;
        uint8_t pc = b->board[SQ(f, r)];
        if (PIECE_TYPE(pc) == PIECE_KNIGHT && PIECE_COLOR(pc) == opp)
            return true;
    }
    /* bishops / queens */
    for (int i = 0; i < 4; i++) {
        int f = kf, r = kr;
        for (;;) {
            f += BISHOP_DIRS[i][0]; r += BISHOP_DIRS[i][1];
            if (f < 0 || f > 7 || r < 0 || r > 7) break;
            uint8_t pc = b->board[SQ(f, r)];
            if (!pc) continue;
            if (PIECE_COLOR(pc) == opp &&
                (PIECE_TYPE(pc) == PIECE_BISHOP ||
                 PIECE_TYPE(pc) == PIECE_QUEEN)) return true;
            break;
        }
    }
    /* rooks / queens */
    for (int i = 0; i < 4; i++) {
        int f = kf, r = kr;
        for (;;) {
            f += ROOK_DIRS[i][0]; r += ROOK_DIRS[i][1];
            if (f < 0 || f > 7 || r < 0 || r > 7) break;
            uint8_t pc = b->board[SQ(f, r)];
            if (!pc) continue;
            if (PIECE_COLOR(pc) == opp &&
                (PIECE_TYPE(pc) == PIECE_ROOK ||
                 PIECE_TYPE(pc) == PIECE_QUEEN)) return true;
            break;
        }
    }
    /* king (for adjacent king attack) */
    for (int i = 0; i < 8; i++) {
        int f = kf + KING_DIRS[i][0];
        int r = kr + KING_DIRS[i][1];
        if (f < 0 || f > 7 || r < 0 || r > 7) continue;
        uint8_t pc = b->board[SQ(f, r)];
        if (PIECE_TYPE(pc) == PIECE_KING && PIECE_COLOR(pc) == opp)
            return true;
    }
    return false;
}

/* ================================================================== */
/*  Make / Unmake move                                                 */
/* ================================================================== */
void board_make_move(Board* b, Move m, Undo* u) {
    u->captured = 0;
    u->castling = b->castling;
    u->ep_sq    = b->ep_sq;
    u->halfmove = b->halfmove;
    u->hash     = b->hash;

    int from = MOVE_FROM(m), to = MOVE_TO(m);
    int flags = MOVE_FLAGS(m);
    uint8_t piece = b->board[from];
    uint8_t cap   = b->board[to];
    int color = PIECE_COLOR(piece);

    /* remove old hash contributions */
    b->hash ^= ZOB_CASTLE[b->castling & 15];
    if (b->ep_sq >= 0) b->hash ^= ZOB_EP[FILE_OF(b->ep_sq)];

    b->halfmove++;
    b->ep_sq = -1;

    if (flags & MF_EP) {
        int cap_sq = SQ(FILE_OF(to), RANK_OF(from));
        u->captured = b->board[cap_sq];
        b->hash ^= ZOB_PIECE[board_piece_idx(u->captured)][cap_sq];
        b->board[cap_sq] = 0;
        b->halfmove = 0;
    } else if (cap) {
        u->captured = cap;
        b->hash ^= ZOB_PIECE[board_piece_idx(cap)][to];
        b->halfmove = 0;
    }

    /* move piece */
    b->hash ^= ZOB_PIECE[board_piece_idx(piece)][from];
    b->board[from] = 0;

    if (flags & MF_PROMO) {
        int promo_type = MOVE_PROMO(m);
        uint8_t promo_piece = MAKE_PIECE(promo_type, color);
        b->board[to] = promo_piece;
        b->hash ^= ZOB_PIECE[board_piece_idx(promo_piece)][to];
    } else {
        b->board[to] = piece;
        b->hash ^= ZOB_PIECE[board_piece_idx(piece)][to];
    }

    /* castling: move rook */
    if (flags & MF_CASTLE) {
        if (to == SQ(6, 0)) {        /* WK-side */
            b->board[SQ(5,0)] = b->board[SQ(7,0)];
            b->hash ^= ZOB_PIECE[board_piece_idx(b->board[SQ(5,0)])][SQ(7,0)];
            b->hash ^= ZOB_PIECE[board_piece_idx(b->board[SQ(5,0)])][SQ(5,0)];
            b->board[SQ(7,0)] = 0;
        } else if (to == SQ(2, 0)) { /* WQ-side */
            b->board[SQ(3,0)] = b->board[SQ(0,0)];
            b->hash ^= ZOB_PIECE[board_piece_idx(b->board[SQ(3,0)])][SQ(0,0)];
            b->hash ^= ZOB_PIECE[board_piece_idx(b->board[SQ(3,0)])][SQ(3,0)];
            b->board[SQ(0,0)] = 0;
        } else if (to == SQ(6, 7)) { /* BK-side */
            b->board[SQ(5,7)] = b->board[SQ(7,7)];
            b->hash ^= ZOB_PIECE[board_piece_idx(b->board[SQ(5,7)])][SQ(7,7)];
            b->hash ^= ZOB_PIECE[board_piece_idx(b->board[SQ(5,7)])][SQ(5,7)];
            b->board[SQ(7,7)] = 0;
        } else if (to == SQ(2, 7)) { /* BQ-side */
            b->board[SQ(3,7)] = b->board[SQ(0,7)];
            b->hash ^= ZOB_PIECE[board_piece_idx(b->board[SQ(3,7)])][SQ(0,7)];
            b->hash ^= ZOB_PIECE[board_piece_idx(b->board[SQ(3,7)])][SQ(3,7)];
            b->board[SQ(0,7)] = 0;
        }
    }

    /* update castling rights */
    if (PIECE_TYPE(piece) == PIECE_KING) {
        if (color == COLOR_WHITE) b->castling &= ~(CR_WK | CR_WQ);
        else                       b->castling &= ~(CR_BK | CR_BQ);
    }
    if (from == SQ(0,0) || to == SQ(0,0)) b->castling &= ~CR_WQ;
    if (from == SQ(7,0) || to == SQ(7,0)) b->castling &= ~CR_WK;
    if (from == SQ(0,7) || to == SQ(0,7)) b->castling &= ~CR_BQ;
    if (from == SQ(7,7) || to == SQ(7,7)) b->castling &= ~CR_BK;

    /* pawn reset halfmove + set ep */
    if (PIECE_TYPE(piece) == PIECE_PAWN) {
        b->halfmove = 0;
        if (abs(RANK_OF(to) - RANK_OF(from)) == 2) {
            b->ep_sq = SQ(FILE_OF(from),
                          (RANK_OF(from) + RANK_OF(to)) / 2);
        }
    }

    /* update hash: castling, ep, stm */
    b->hash ^= ZOB_CASTLE[b->castling & 15];
    if (b->ep_sq >= 0) b->hash ^= ZOB_EP[FILE_OF(b->ep_sq)];
    b->hash ^= ZOB_STM;

    b->stm ^= 1;
    if (b->stm == COLOR_WHITE) b->fullmove++;
}

void board_unmake_move(Board* b, Move m, const Undo* u) {
    b->stm      = b->stm ^ 1;
    b->castling = u->castling;
    b->ep_sq    = u->ep_sq;
    b->halfmove = u->halfmove;
    b->hash     = u->hash;
    if (b->stm == COLOR_BLACK && b->fullmove > 1) b->fullmove--;

    int from = MOVE_FROM(m), to = MOVE_TO(m);
    int flags = MOVE_FLAGS(m);
    int color = b->stm;

    uint8_t moved;
    if (flags & MF_PROMO) {
        moved = MAKE_PIECE(PIECE_PAWN, color);
    } else {
        moved = b->board[to];
    }
    b->board[from] = moved;
    b->board[to]   = 0;

    if (flags & MF_EP) {
        int cap_sq = SQ(FILE_OF(to), RANK_OF(from));
        b->board[cap_sq] = u->captured;
    } else if (u->captured) {
        b->board[to] = u->captured;
    }

    /* restore rooks after castling */
    if (flags & MF_CASTLE) {
        if (to == SQ(6, 0)) {
            b->board[SQ(7,0)] = b->board[SQ(5,0)]; b->board[SQ(5,0)] = 0;
        } else if (to == SQ(2, 0)) {
            b->board[SQ(0,0)] = b->board[SQ(3,0)]; b->board[SQ(3,0)] = 0;
        } else if (to == SQ(6, 7)) {
            b->board[SQ(7,7)] = b->board[SQ(5,7)]; b->board[SQ(5,7)] = 0;
        } else if (to == SQ(2, 7)) {
            b->board[SQ(0,7)] = b->board[SQ(3,7)]; b->board[SQ(3,7)] = 0;
        }
    }
}

/* ================================================================== */
/*  Pseudo-legal move generation                                       */
/* ================================================================== */
static void add_move(MoveList* ml, int from, int to, int flags, int promo) {
    ml->moves[ml->count++] = make_move(from, to, promo, flags);
}

static void gen_pawn_moves(const Board* b, MoveList* ml, int color) {
    int dir    = (color == COLOR_WHITE) ? 1 : -1;
    int rank7  = (color == COLOR_WHITE) ? 6 : 1;  /* rank before promo  */
    int rank2  = (color == COLOR_WHITE) ? 1 : 6;  /* starting rank      */
    uint8_t pc = MAKE_PIECE(PIECE_PAWN, color);

    for (int sq = 0; sq < 64; sq++) {
        if (b->board[sq] != pc) continue;
        int f = FILE_OF(sq), r = RANK_OF(sq);
        int r1 = r + dir;

        /* single push */
        if (r1 >= 0 && r1 <= 7 && !b->board[SQ(f, r1)]) {
            if (r == rank7) {
                for (int p = PIECE_KNIGHT; p <= PIECE_QUEEN; p++)
                    add_move(ml, sq, SQ(f, r1), MF_PROMO, p);
            } else {
                add_move(ml, sq, SQ(f, r1), MF_NORMAL, 0);
                /* double push */
                int r2 = r1 + dir;
                if (r == rank2 && !b->board[SQ(f, r2)])
                    add_move(ml, sq, SQ(f, r2), MF_NORMAL, 0);
            }
        }
        /* captures */
        for (int df = -1; df <= 1; df += 2) {
            int f2 = f + df;
            if (f2 < 0 || f2 > 7 || r1 < 0 || r1 > 7) continue;
            int to_sq = SQ(f2, r1);
            uint8_t cap = b->board[to_sq];
            if (cap && PIECE_COLOR(cap) != color) {
                if (r == rank7) {
                    for (int p = PIECE_KNIGHT; p <= PIECE_QUEEN; p++)
                        add_move(ml, sq, to_sq, MF_PROMO, p);
                } else {
                    add_move(ml, sq, to_sq, MF_NORMAL, 0);
                }
            }
            /* en passant */
            if (b->ep_sq == to_sq)
                add_move(ml, sq, to_sq, MF_EP, 0);
        }
    }
}

static void gen_knight_moves(const Board* b, MoveList* ml, int color) {
    uint8_t pc = MAKE_PIECE(PIECE_KNIGHT, color);
    for (int sq = 0; sq < 64; sq++) {
        if (b->board[sq] != pc) continue;
        int f = FILE_OF(sq), r = RANK_OF(sq);
        for (int i = 0; i < 8; i++) {
            int f2 = f + KNIGHT_OFFSETS[i][0];
            int r2 = r + KNIGHT_OFFSETS[i][1];
            if (f2 < 0 || f2 > 7 || r2 < 0 || r2 > 7) continue;
            int to = SQ(f2, r2);
            if (!b->board[to] || PIECE_COLOR(b->board[to]) != color)
                add_move(ml, sq, to, MF_NORMAL, 0);
        }
    }
}

static void gen_slider_moves(const Board* b, MoveList* ml, int color,
                              int type,
                              const int dirs[][2], int ndirs) {
    uint8_t pc = MAKE_PIECE(type, color);
    for (int sq = 0; sq < 64; sq++) {
        if (b->board[sq] != pc) continue;
        int f0 = FILE_OF(sq), r0 = RANK_OF(sq);
        for (int d = 0; d < ndirs; d++) {
            int f = f0, r = r0;
            for (;;) {
                f += dirs[d][0]; r += dirs[d][1];
                if (f < 0 || f > 7 || r < 0 || r > 7) break;
                int to = SQ(f, r);
                if (b->board[to]) {
                    if (PIECE_COLOR(b->board[to]) != color)
                        add_move(ml, sq, to, MF_NORMAL, 0);
                    break;
                }
                add_move(ml, sq, to, MF_NORMAL, 0);
            }
        }
    }
}

static void gen_king_moves(const Board* b, MoveList* ml, int color) {
    uint8_t pc = MAKE_PIECE(PIECE_KING, color);
    for (int ksq = 0; ksq < 64; ksq++) {
        if (b->board[ksq] != pc) continue;
        int f0 = FILE_OF(ksq), r0 = RANK_OF(ksq);
        for (int i = 0; i < 8; i++) {
            int f = f0 + KING_DIRS[i][0];
            int r = r0 + KING_DIRS[i][1];
            if (f < 0 || f > 7 || r < 0 || r > 7) continue;
            int to = SQ(f, r);
            if (!b->board[to] || PIECE_COLOR(b->board[to]) != color)
                add_move(ml, ksq, to, MF_NORMAL, 0);
        }
        /* castling — king must be on its home square */
        if (color == COLOR_WHITE && ksq == SQ(4, 0)) {
            if ((b->castling & CR_WK) &&
                !b->board[SQ(5,0)] && !b->board[SQ(6,0)])
                add_move(ml, ksq, SQ(6,0), MF_CASTLE, 0);
            if ((b->castling & CR_WQ) &&
                !b->board[SQ(3,0)] && !b->board[SQ(2,0)] && !b->board[SQ(1,0)])
                add_move(ml, ksq, SQ(2,0), MF_CASTLE, 0);
        } else if (color == COLOR_BLACK && ksq == SQ(4, 7)) {
            if ((b->castling & CR_BK) &&
                !b->board[SQ(5,7)] && !b->board[SQ(6,7)])
                add_move(ml, ksq, SQ(6,7), MF_CASTLE, 0);
            if ((b->castling & CR_BQ) &&
                !b->board[SQ(3,7)] && !b->board[SQ(2,7)] && !b->board[SQ(1,7)])
                add_move(ml, ksq, SQ(2,7), MF_CASTLE, 0);
        }
    }
}

void board_gen_moves(const Board* b, MoveList* ml) {
    ml->count = 0;
    int color = b->stm;
    gen_pawn_moves(b, ml, color);
    gen_knight_moves(b, ml, color);
    gen_slider_moves(b, ml, color, PIECE_BISHOP,
                     BISHOP_DIRS, 4);
    gen_slider_moves(b, ml, color, PIECE_ROOK,
                     ROOK_DIRS, 4);
    /* queen = bishop + rook directions */
    gen_slider_moves(b, ml, color, PIECE_QUEEN,
                     BISHOP_DIRS, 4);
    gen_slider_moves(b, ml, color, PIECE_QUEEN,
                     ROOK_DIRS, 4);
    gen_king_moves(b, ml, color);
}

void board_gen_legal(Board* b, MoveList* ml) {
    MoveList pseudo;
    board_gen_moves(b, &pseudo);
    ml->count = 0;
    for (int i = 0; i < pseudo.count; i++) {
        Move m = pseudo.moves[i];
        Undo u;
        board_make_move(b, m, &u);
        /* also check castling passes through check */
        bool legal = !board_in_check(b, b->stm ^ 1);
        if (legal && IS_CASTLE(m)) {
            /* verify king doesn't pass through attacked square */
            int dir = (MOVE_TO(m) > MOVE_FROM(m)) ? 1 : -1;
            Board tmp2 = *b;
            board_unmake_move(&tmp2, m, &u);
            /* check intermediate square */
            Board mid = tmp2;
            Undo u2;
            Move mid_m = make_move(MOVE_FROM(m),
                                   MOVE_FROM(m) + dir, 0, MF_NORMAL);
            /* pseudo check: just see if king passes through */
            mid.board[MOVE_FROM(m) + dir] = mid.board[MOVE_FROM(m)];
            mid.board[MOVE_FROM(m)] = 0;
            if (board_in_check(&mid, tmp2.stm)) legal = false;
            (void)u2; (void)mid_m;
        }
        board_unmake_move(b, m, &u);
        if (legal) ml->moves[ml->count++] = m;
    }
}

int board_game_result(Board* b) {
    MoveList ml;
    board_gen_legal(b, &ml);
    if (ml.count > 0) {
        if (b->halfmove >= 100) return GAME_DRAW;
        return GAME_ONGOING;
    }
    if (board_in_check(b, b->stm)) {
        return (b->stm == COLOR_WHITE) ? GAME_BLACK_WIN : GAME_WHITE_WIN;
    }
    return GAME_DRAW;   /* stalemate */
}

/* ================================================================== */
/*  Move parsing / string conversion                                   */
/* ================================================================== */
void board_move_str(Move m, char* out) {
    int from = MOVE_FROM(m), to = MOVE_TO(m);
    out[0] = (char)('a' + FILE_OF(from));
    out[1] = (char)('1' + RANK_OF(from));
    out[2] = (char)('a' + FILE_OF(to));
    out[3] = (char)('1' + RANK_OF(to));
    if (IS_PROMO(m)) {
        static const char ptypes[] = ".pnbrq";
        out[4] = ptypes[MOVE_PROMO(m)];
        out[5] = '\0';
    } else {
        out[4] = '\0';
    }
}

Move board_parse_move(const Board* b, const char* str) {
    /* accept UCI: e2e4, e7e8q */
    char s[8];
    int i = 0;
    while (str[i] && !isspace((unsigned char)str[i]) && i < 7)
        { s[i] = (char)tolower((unsigned char)str[i]); i++; }
    s[i] = '\0';

    if (i < 4) return MOVE_NONE;
    int from = SQ(s[0]-'a', s[1]-'1');
    int to   = SQ(s[2]-'a', s[3]-'1');
    int promo = 0;
    if (i == 5) {
        const char* pc = "pnbrq";
        const char* found = strchr(pc, s[4]);
        promo = found ? (int)(found - pc) + 1 : 0;
    }

    MoveList ml;
    board_gen_legal((Board*)b, &ml);
    for (int j = 0; j < ml.count; j++) {
        Move m = ml.moves[j];
        if (MOVE_FROM(m) == from && MOVE_TO(m) == to) {
            if (IS_PROMO(m)) {
                if (MOVE_PROMO(m) == promo) return m;
            } else {
                return m;
            }
        }
    }
    return MOVE_NONE;
}

/* ================================================================== */
/*  Feature extraction for NNUE (768-float HalfKP-lite)              */
/* ================================================================== */
void board_get_features(const Board* b, float* feat) {
    memset(feat, 0, NNUE_FEATURES * sizeof(float));
    for (int sq = 0; sq < 64; sq++) {
        uint8_t pc = b->board[sq];
        if (!pc) continue;
        int idx = board_piece_idx(pc); /* 0-11 */
        feat[idx * 64 + sq] = 1.0f;
    }
}