/*  move_lawyer.c — Explains (in plain English) why a UCI move is illegal.

    Checks are layered in order of specificity:
      1.  Format / parse errors
      2.  Empty source square
      3.  Wrong side's piece
      4.  Same-square no-op
      5.  Friendly capture (target is own piece)
      6.  Piece-movement pattern violations
          - Pawn  : wrong direction, can't push into occupied sq, diagonal
                    without a capture target, double-push through a piece,
                    double-push from non-starting rank
          - Knight: not an L-shape
          - Bishop: not diagonal
          - Rook  : not horizontal/vertical
          - Queen : not horizontal/vertical/diagonal
          - King  : too many squares (non-castling), OR detailed castling
                    breakdown (rights lost, path blocked, through check…)
          - Slider path obstructions are each reported individually
      7.  Move leaves own king in check (including "already in check and
          this move doesn't resolve it")                                        */

#include "move_lawyer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>

/* ── helpers ──────────────────────────────────────────────────────────────── */

static const char* PIECE_NAME[] = {
    "", "pawn", "knight", "bishop", "rook", "queen", "king"
};

static const char* COLOR_STR[] = { "White", "Black" };

static void sq_str(int sq, char out[3]) {
    out[0] = (char)('a' + FILE_OF(sq));
    out[1] = (char)('1' + RANK_OF(sq));
    out[2] = '\0';
}

/* Append a bullet-point reason to the growing output buffer. */
static void add_reason(char* out, int out_size, const char* fmt, ...) {
    va_list ap;
    char    tmp[512];
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    int         cur    = (int)strlen(out);
    const char* prefix = (cur == 0) ? "  - " : "\n  - ";
    int         plen   = (int)strlen(prefix);
    int         tlen   = (int)strlen(tmp);

    if (cur + plen + tlen < out_size - 1) {
        memcpy(out + cur,        prefix, (size_t)plen);
        memcpy(out + cur + plen, tmp,    (size_t)(tlen + 1));
    }
}

/* ── path-obstruction helper ──────────────────────────────────────────────── */

/*  Report every piece blocking the ray from (ff,rf) to (ft,rt).
    (sf,sr) is the unit step along the ray; `steps` is the ray length.        */
static void check_path(const Board* b, char* out, int out_size,
                       int ff, int rf, int sf, int sr, int steps)
{
    for (int s = 1; s < steps; s++) {
        int sq = SQ(ff + s * sf, rf + s * sr);
        if (b->board[sq]) {
            char bsq[3]; sq_str(sq, bsq);
            add_reason(out, out_size,
                "The path is blocked: a %s %s is sitting on %s.",
                COLOR_STR[PIECE_COLOR(b->board[sq])],
                PIECE_NAME[PIECE_TYPE(b->board[sq])],
                bsq);
        }
    }
}

/* ── "leaves king in check" helper ───────────────────────────────────────── */

/*  Make the move on a scratch board (no move-flag magic — just relocate the
    piece and optionally remove an en-passant capture) then ask board_in_check.
    This works even for pseudo-illegal moves.                                  */
static bool leaves_king_in_check(const Board* b,
                                  int from, int to, int color)
{
    Board tmp = *b;
    int   ptype = PIECE_TYPE(tmp.board[from]);

    /* En-passant: remove the captured pawn on the side rank. */
    if (ptype == PIECE_PAWN &&
        FILE_OF(from) != FILE_OF(to) &&   /* diagonal move */
        !tmp.board[to]               &&   /* destination empty → en passant */
        tmp.ep_sq == to)
    {
        tmp.board[SQ(FILE_OF(to), RANK_OF(from))] = 0;
    }

    tmp.board[to]   = tmp.board[from];
    tmp.board[from] = 0;

    return board_in_check(&tmp, color);
}

/* ── castling diagnosis ───────────────────────────────────────────────────── */

static void check_castling(const Board* b, char* out, int out_size,
                            int from, int to, int color)
{
    int df  = FILE_OF(to) - FILE_OF(from);
    int dir = (df > 0) ? 1 : -1;

    /* King home square */
    int king_home = (color == COLOR_WHITE) ? SQ(4, 0) : SQ(4, 7);
    if (from != king_home) {
        add_reason(out, out_size,
            "Castling is impossible: the king has already left its home square.");
    }

    /* Castling rights */
    int cr_needed, rook_sq;
    if (color == COLOR_WHITE) {
        if (dir > 0) { cr_needed = CR_WK; rook_sq = SQ(7, 0); }
        else         { cr_needed = CR_WQ; rook_sq = SQ(0, 0); }
    } else {
        if (dir > 0) { cr_needed = CR_BK; rook_sq = SQ(7, 7); }
        else         { cr_needed = CR_BQ; rook_sq = SQ(0, 7); }
    }

    if (!(b->castling & cr_needed)) {
        add_reason(out, out_size,
            "Castling rights on this side have been forfeited — either the king "
            "or the relevant rook has previously moved or been captured.");
    }

    /* Rook still present? */
    int rank = RANK_OF(from);
    if (!b->board[rook_sq] ||
        PIECE_TYPE(b->board[rook_sq]) != PIECE_ROOK ||
        PIECE_COLOR(b->board[rook_sq]) != color)
    {
        char rsq[3]; sq_str(rook_sq, rsq);
        add_reason(out, out_size,
            "There is no %s rook on %s to castle with.",
            COLOR_STR[color], rsq);
    }

    /* All squares between king and rook must be empty. */
    int kf = FILE_OF(from), rf_rook = FILE_OF(rook_sq);
    int lo = (kf < rf_rook ? kf : rf_rook) + 1;
    int hi = (kf < rf_rook ? rf_rook : kf) - 1;
    for (int f = lo; f <= hi; f++) {
        int sq = SQ(f, rank);
        if (b->board[sq]) {
            char bsq[3]; sq_str(sq, bsq);
            add_reason(out, out_size,
                "The square %s (between king and rook) is occupied by a %s %s.",
                bsq,
                COLOR_STR[PIECE_COLOR(b->board[sq])],
                PIECE_NAME[PIECE_TYPE(b->board[sq])]);
        }
    }

    /* King must not currently be in check. */
    if (board_in_check(b, color)) {
        add_reason(out, out_size,
            "You cannot castle while your king is in check.");
    }

    /* King must not pass through an attacked square. */
    int pass_sq = SQ(FILE_OF(from) + dir, rank);
    {
        Board mid        = *b;
        mid.board[pass_sq] = mid.board[from];
        mid.board[from]    = 0;
        if (board_in_check(&mid, color)) {
            char psq[3]; sq_str(pass_sq, psq);
            add_reason(out, out_size,
                "The king would pass through %s, which is controlled by an enemy piece.",
                psq);
        }
    }

    /* King must not end up in check. */
    int dest_king_sq = SQ(FILE_OF(from) + 2 * dir, rank);
    {
        Board end             = *b;
        end.board[dest_king_sq] = end.board[from];
        end.board[from]         = 0;
        /* Clear the pass square so the rook's final square doesn't confuse
           board_in_check's king-adjacency search. */
        end.board[pass_sq] = 0;
        if (board_in_check(&end, color)) {
            char dsq[3]; sq_str(dest_king_sq, dsq);
            add_reason(out, out_size,
                "The king would land on %s, which is under attack.", dsq);
        }
    }
}

/* ── main public function ─────────────────────────────────────────────────── */

void move_lawyer_explain(const Board* b,
                         const char*  move_str,
                         char*        out,
                         int          out_size)
{
    out[0] = '\0';

    /* ── 1. Parse UCI string ─────────────────────────────────────────────── */

    /* Skip leading whitespace. */
    while (*move_str == ' ' || *move_str == '\n' || *move_str == '\r')
        move_str++;

    int len = 0;
    while (move_str[len] && !isspace((unsigned char)move_str[len]) && len < 8)
        len++;

    if (len < 4) {
        add_reason(out, out_size,
            "Move string \"%s\" is too short. Expected UCI format like \"e2e4\" "
            "(source square then destination square, e.g. e2e4, g8f6, e7e8q).",
            move_str);
        return;
    }

    char s[8] = {0};
    for (int i = 0; i < len && i < 7; i++)
        s[i] = (char)tolower((unsigned char)move_str[i]);

    /* Validate individual characters. */
    if (s[0] < 'a' || s[0] > 'h' || s[2] < 'a' || s[2] > 'h') {
        add_reason(out, out_size,
            "File characters must be a–h. Got '%c'→'%c'.", s[0], s[2]);
        return;
    }
    if (s[1] < '1' || s[1] > '8' || s[3] < '1' || s[3] > '8') {
        add_reason(out, out_size,
            "Rank characters must be 1–8. Got '%c'→'%c'.", s[1], s[3]);
        return;
    }

    int from = SQ(s[0] - 'a', s[1] - '1');
    int to   = SQ(s[2] - 'a', s[3] - '1');

    /* Promotion character (optional 5th char). */
    int promo_requested = 0;
    if (len == 5) {
        const char* legal_promos = "nbrq";
        const char* found        = strchr(legal_promos, s[4]);
        if (!found) {
            add_reason(out, out_size,
                "'%c' is not a valid promotion piece. Use n, b, r, or q.",
                s[4]);
        } else {
            promo_requested = (int)(found - legal_promos) + PIECE_KNIGHT;
        }
    }

    char fsq[3], tsq[3];
    sq_str(from, fsq);
    sq_str(to,   tsq);

    /* ── 2. Source square empty? ─────────────────────────────────────────── */

    uint8_t piece  = b->board[from];
    uint8_t target = b->board[to];
    int     color  = b->stm;

    if (!piece) {
        add_reason(out, out_size,
            "There is no piece on %s to move.", fsq);
        return;
    }

    int ptype  = PIECE_TYPE(piece);
    int pcolor = PIECE_COLOR(piece);

    /* ── 3. Wrong side's piece ───────────────────────────────────────────── */

    if (pcolor != color) {
        add_reason(out, out_size,
            "The %s on %s belongs to %s, but it is %s's turn to move.",
            PIECE_NAME[ptype], fsq, COLOR_STR[pcolor], COLOR_STR[color]);
        /* Nothing further is relevant when moving the opponent's piece. */
        return;
    }

    /* ── 4. Same-square no-op ────────────────────────────────────────────── */

    if (from == to) {
        add_reason(out, out_size,
            "The source and destination squares are the same (%s). "
            "A move must change the piece's position.", fsq);
        return;
    }

    /* ── 5. Friendly capture ─────────────────────────────────────────────── */

    bool is_castle_attempt = (ptype == PIECE_KING && abs(FILE_OF(to) - FILE_OF(from)) == 2
                                                  && RANK_OF(from) == RANK_OF(to));

    if (target && PIECE_COLOR(target) == color && !is_castle_attempt) {
        add_reason(out, out_size,
            "The destination %s is occupied by your own %s. "
            "You cannot capture your own pieces.",
            tsq, PIECE_NAME[PIECE_TYPE(target)]);
    }

    /* ── 6. Piece-movement pattern ───────────────────────────────────────── */

    int ff  = FILE_OF(from), rf = RANK_OF(from);
    int ft  = FILE_OF(to),   rt = RANK_OF(to);
    int df  = ft - ff,       dr = rt - rf;
    int adf = abs(df),       adr = abs(dr);

    switch (ptype) {

    /* ── PAWN ────────────────────────────────────────────────────────────── */
    case PIECE_PAWN: {
        int dir        = (color == COLOR_WHITE) ? 1 : -1;
        int start_rank = (color == COLOR_WHITE) ? 1 : 6;

        if (df == 0) {
            /* Forward or backward straight moves. */
            if (dr == -dir) {
                add_reason(out, out_size,
                    "Pawns can only move forward, never backward. "
                    "This %s pawn on %s would be moving in the wrong direction.",
                    COLOR_STR[color], fsq);
            } else if (adr > 2 || (adr == 2 && dr != 2 * dir)) {
                add_reason(out, out_size,
                    "Pawns can advance at most two squares (from the starting rank only). "
                    "Moving %d squares is not permitted.", adr);
            } else if (adr == 2 && dr == 2 * dir) {
                /* Double push: starting rank + clear path. */
                if (rf != start_rank) {
                    add_reason(out, out_size,
                        "The two-square pawn advance is only available from the starting rank. "
                        "The pawn on %s has already moved.", fsq);
                }
                int mid_sq = SQ(ff, rf + dir);
                if (b->board[mid_sq]) {
                    char msq[3]; sq_str(mid_sq, msq);
                    add_reason(out, out_size,
                        "The pawn on %s cannot advance two squares because %s is "
                        "blocked by a %s %s.",
                        fsq, msq,
                        COLOR_STR[PIECE_COLOR(b->board[mid_sq])],
                        PIECE_NAME[PIECE_TYPE(b->board[mid_sq])]);
                }
                if (target) {
                    add_reason(out, out_size,
                        "The destination %s is occupied — pawns cannot push into "
                        "an occupied square.", tsq);
                }
            } else {
                /* Single push (dr == dir). */
                if (target) {
                    add_reason(out, out_size,
                        "Pawns cannot capture pieces directly in front of them — "
                        "they can only capture diagonally. The square %s is occupied "
                        "by a %s %s.",
                        tsq,
                        COLOR_STR[PIECE_COLOR(target)],
                        PIECE_NAME[PIECE_TYPE(target)]);
                }
            }
        } else if (adf == 1 && dr == dir) {
            /* Diagonal (capture) move. */
            if (!target && b->ep_sq != to) {
                add_reason(out, out_size,
                    "Pawns can only move diagonally when capturing an enemy piece "
                    "(or via en passant). There is no enemy piece to capture on %s, "
                    "and en passant is not available to %s right now.", tsq, tsq);
            }
            /* Friendly diagonal capture already caught in §5. */
        } else {
            /* Any other geometry. */
            if (dr == -dir) {
                add_reason(out, out_size,
                    "Pawns cannot move backward, diagonally or otherwise.");
            } else if (adf > 1) {
                add_reason(out, out_size,
                    "Pawns can only move within the same file (straight) or one "
                    "file across (diagonal capture). Moving %d files is illegal.",
                    adf);
            } else {
                add_reason(out, out_size,
                    "This is not a legal pawn move from %s to %s. "
                    "Pawns move one square forward (or two from the starting rank) "
                    "and capture one square diagonally.",
                    fsq, tsq);
            }
        }

        /* Missing promotion specifier. */
        if (ptype == PIECE_PAWN) {
            int promo_rank = (color == COLOR_WHITE) ? 7 : 0;
            if (rt == promo_rank && len < 5) {
                add_reason(out, out_size,
                    "Pawn reaching the back rank must declare a promotion piece. "
                    "Append n, b, r, or q — e.g. \"%s%sq\".", fsq, tsq);
            }
        }
        break;
    }

    /* ── KNIGHT ──────────────────────────────────────────────────────────── */
    case PIECE_KNIGHT: {
        bool is_l = (adf == 1 && adr == 2) || (adf == 2 && adr == 1);
        if (!is_l) {
            add_reason(out, out_size,
                "Knights move in an 'L'-shape: two squares in one direction then "
                "one square perpendicular (or vice versa). %s→%s spans %d file(s) "
                "and %d rank(s), which is not an L-shape.",
                fsq, tsq, adf, adr);
        }
        /* Knights jump — no path-blocking check needed. */
        break;
    }

    /* ── BISHOP ──────────────────────────────────────────────────────────── */
    case PIECE_BISHOP: {
        if (adf != adr || adf == 0) {
            add_reason(out, out_size,
                "Bishops can only move diagonally (equal file and rank distance). "
                "%s→%s spans %d file(s) and %d rank(s) — not a diagonal.",
                fsq, tsq, adf, adr);
        } else {
            int sf = (df > 0) ? 1 : -1;
            int sr = (dr > 0) ? 1 : -1;
            check_path(b, out, out_size, ff, rf, sf, sr, adf);
        }
        break;
    }

    /* ── ROOK ────────────────────────────────────────────────────────────── */
    case PIECE_ROOK: {
        if (df != 0 && dr != 0) {
            add_reason(out, out_size,
                "Rooks can only move horizontally or vertically (along a rank or "
                "file). %s→%s moves %d file(s) and %d rank(s) — not straight.",
                fsq, tsq, adf, adr);
        } else {
            int sf    = (df == 0) ? 0 : (df > 0 ? 1 : -1);
            int sr    = (dr == 0) ? 0 : (dr > 0 ? 1 : -1);
            int steps = (df == 0) ? adr : adf;
            check_path(b, out, out_size, ff, rf, sf, sr, steps);
        }
        break;
    }

    /* ── QUEEN ───────────────────────────────────────────────────────────── */
    case PIECE_QUEEN: {
        bool is_diag     = (adf == adr && adf > 0);
        bool is_straight = (df == 0 || dr == 0) && (adf + adr > 0);

        if (!is_diag && !is_straight) {
            add_reason(out, out_size,
                "Queens can move horizontally, vertically, or diagonally, but not "
                "in arbitrary directions. %s→%s spans %d file(s) and %d rank(s), "
                "which matches none of those patterns.",
                fsq, tsq, adf, adr);
        } else {
            int sf, sr, steps;
            if (is_diag) {
                sf = (df > 0) ? 1 : -1;
                sr = (dr > 0) ? 1 : -1;
                steps = adf;
            } else {
                sf    = (df == 0) ? 0 : (df > 0 ? 1 : -1);
                sr    = (dr == 0) ? 0 : (dr > 0 ? 1 : -1);
                steps = (df == 0) ? adr : adf;
            }
            check_path(b, out, out_size, ff, rf, sf, sr, steps);
        }
        break;
    }

    /* ── KING ────────────────────────────────────────────────────────────── */
    case PIECE_KING: {
        if (is_castle_attempt) {
            check_castling(b, out, out_size, from, to, color);
        } else if (adf > 1 || adr > 1) {
            add_reason(out, out_size,
                "Kings can only move one square in any direction (unless castling, "
                "which requires moving exactly two squares sideways). "
                "%s→%s is %d square(s) away.",
                fsq, tsq, (adf > adr ? adf : adr));
        }
        break;
    }

    } /* switch */

    /* ── 7. Leaves own king in check ─────────────────────────────────────── */

    if (leaves_king_in_check(b, from, to, color)) {
        if (board_in_check(b, color)) {
            add_reason(out, out_size,
                "Your king is currently in check and this move does not resolve it. "
                "You must block the check, capture the attacking piece, or move the king.");
        } else {
            if (ptype == PIECE_KING) {
                add_reason(out, out_size,
                    "The king cannot move to %s because it would be in check there.", tsq);
            } else {
                add_reason(out, out_size,
                    "This move would expose your king to check. "
                    "The %s on %s is pinned — moving it leaves your king undefended.",
                    PIECE_NAME[ptype], fsq);
            }
        }
    }

    /* ── Fallback (should rarely trigger) ────────────────────────────────── */

    if (out[0] == '\0') {
        add_reason(out, out_size,
            "This move is not legal, but the specific violation could not be "
            "identified automatically. Please check the board position carefully.");
    }
}