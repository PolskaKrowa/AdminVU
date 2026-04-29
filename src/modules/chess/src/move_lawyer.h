/*  move_lawyer.h — Explains (in plain English) why a UCI move is illegal.
    Internal header; depends on chess.h.  Not part of the public API.         */
#ifndef MOVE_LAWYER_H
#define MOVE_LAWYER_H

#include "chess.h"

/*  Fills `out` with a human-readable, bullet-pointed list of every reason
    why `move_str` (UCI, e.g. "e2e4") is illegal on board `b`.
    If the string itself is malformed the format error is explained instead.
    `out` is always NUL-terminated.                                            */
void move_lawyer_explain(const Board*  b,
                         const char*   move_str,
                         char*         out,
                         int           out_size);

#endif /* MOVE_LAWYER_H */