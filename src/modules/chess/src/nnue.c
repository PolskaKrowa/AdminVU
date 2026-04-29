#define _POSIX_C_SOURCE 200112L
/*  nnue.c — CPU-side NNUE: weight management, accumulator updates,
    forward pass, gradient application.                              */
#include "nnue.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* ================================================================== */
/*  Helpers                                                            */
/* ================================================================== */
static inline float relu(float x) { return x > 0.0f ? x : 0.0f; }

/* LCG-based fast pseudo-random for weight init */
static float rand_glorot(uint64_t* s, int fan_in, int fan_out) {
    /* xorshift64 */
    *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17;
    double u = (double)(*s >> 11) / (double)(1ULL << 53);
    /* uniform Glorot: range ±sqrt(6/(fan_in+fan_out)) */
    double limit = sqrt(6.0 / (fan_in + fan_out));
    return (float)((u * 2.0 - 1.0) * limit);
}

/* ================================================================== */
/*  Allocation                                                         */
/* ================================================================== */
NNUEWeights* nnue_alloc_weights(void) {
#ifdef _WIN32
    NNUEWeights* w = (NNUEWeights*)_aligned_malloc(sizeof(NNUEWeights), NNUE_ALIGN);
#else
    NNUEWeights* w = NULL;
    if (posix_memalign((void**)&w, NNUE_ALIGN, sizeof(NNUEWeights)) != 0)
        w = NULL;
#endif
    if (w) memset(w, 0, sizeof(NNUEWeights));
    return w;
}

void nnue_free_weights(NNUEWeights* w) {
    if (!w) return;
#ifdef _WIN32
    _aligned_free(w);
#else
    free(w);
#endif
}

/* ================================================================== */
/*  Random initialisation                                              */
/* ================================================================== */
void nnue_init_random(NNUEWeights* w, uint64_t seed) {
    uint64_t s = seed ? seed : 0xABCDEF1234567890ull;

    for (int i = 0; i < NNUE_INPUT; i++)
        for (int j = 0; j < NNUE_L1; j++)
            w->w0[i][j] = rand_glorot(&s, NNUE_INPUT, NNUE_L1);
    for (int j = 0; j < NNUE_L1; j++) w->b0[j] = 0.0f;

    for (int i = 0; i < NNUE_L1 * 2; i++)
        for (int j = 0; j < NNUE_L2; j++)
            w->w1[i][j] = rand_glorot(&s, NNUE_L1 * 2, NNUE_L2);
    for (int j = 0; j < NNUE_L2; j++) w->b1[j] = 0.0f;

    for (int i = 0; i < NNUE_L2; i++)
        for (int j = 0; j < NNUE_L3; j++)
            w->w2[i][j] = rand_glorot(&s, NNUE_L2, NNUE_L3);
    for (int j = 0; j < NNUE_L3; j++) w->b2[j] = 0.0f;

    for (int i = 0; i < NNUE_L3; i++)
        w->w3[i][0] = rand_glorot(&s, NNUE_L3, NNUE_OUTPUT);
    w->b3[0] = 0.0f;
}

/* ================================================================== */
/*  Save / load (simple binary format)                                 */
/* ================================================================== */
bool nnue_save(const NNUEWeights* w, const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) { perror("nnue_save"); return false; }
    uint32_t magic = 0x4E4E5545u;  /* 'NNUE' */
    fwrite(&magic, 4, 1, f);
    fwrite(w, sizeof(NNUEWeights), 1, f);
    fclose(f);
    return true;
}

bool nnue_load(NNUEWeights* w, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror("nnue_load"); return false; }
    uint32_t magic = 0;
    if (fread(&magic, 4, 1, f) != 1 || magic != 0x4E4E5545u) {
        fprintf(stderr, "nnue_load: bad magic in '%s'\n", path);
        fclose(f); return false;
    }
    if (fread(w, sizeof(NNUEWeights), 1, f) != 1) {
        fprintf(stderr, "nnue_load: truncated file '%s'\n", path);
        fclose(f); return false;
    }
    fclose(f);
    return true;
}

/* ================================================================== */
/*  Accumulator refresh (full recompute from features)                */
/* ================================================================== */
void nnue_refresh(NNUEContext* ctx, const float* features) {
    const NNUEWeights* w = ctx->weights;

    /* white perspective */
    for (int j = 0; j < NNUE_L1; j++) {
        float acc = w->b0[j];
        for (int i = 0; i < NNUE_INPUT; i++)
            acc += features[i] * w->w0[i][j];
        ctx->acc.white[j] = acc;
    }
    /* black perspective: mirror square indices (flip rank) */
    for (int j = 0; j < NNUE_L1; j++) {
        float acc = w->b0[j];
        for (int i = 0; i < NNUE_INPUT; i++) {
            /* piece-type stays, square flips: sq ^ 56 */
            int piece_blk = i / 64;
            int sq        = i % 64;
            int mirror_i  = piece_blk * 64 + (sq ^ 56);
            acc += features[mirror_i] * w->w0[i][j];
        }
        ctx->acc.black[j] = acc;
    }
}

/* ================================================================== */
/*  Incremental accumulator update                                     */
/* ================================================================== */
void nnue_update_feature(NNUEContext* ctx, int feature_idx,
                          int color_pov, bool add) {
    /*  color_pov: COLOR_WHITE or COLOR_BLACK                          */
    const NNUEWeights* w = ctx->weights;
    float* acc = (color_pov == 0) ? ctx->acc.white : ctx->acc.black;
    float  sign = add ? 1.0f : -1.0f;
    for (int j = 0; j < NNUE_L1; j++)
        acc[j] += sign * w->w0[feature_idx][j];
}

/* ================================================================== */
/*  CPU forward pass                                                   */
/* ================================================================== */
float nnue_evaluate(const NNUEContext* ctx, int side_to_move) {
    const NNUEWeights* w = ctx->weights;
    float l1[NNUE_L1 * 2];

    /* stm perspective first, then opponent */
    const float* pov0 = (side_to_move == 0) ? ctx->acc.white : ctx->acc.black;
    const float* pov1 = (side_to_move == 0) ? ctx->acc.black : ctx->acc.white;

    for (int j = 0; j < NNUE_L1; j++) l1[j]          = relu(pov0[j]);
    for (int j = 0; j < NNUE_L1; j++) l1[j + NNUE_L1] = relu(pov1[j]);

    /* L1 → L2 */
    float l2[NNUE_L2];
    for (int j = 0; j < NNUE_L2; j++) {
        float acc = w->b1[j];
        for (int i = 0; i < NNUE_L1 * 2; i++)
            acc += l1[i] * w->w1[i][j];
        l2[j] = relu(acc);
    }

    /* L2 → L3 */
    float l3[NNUE_L3];
    for (int j = 0; j < NNUE_L3; j++) {
        float acc = w->b2[j];
        for (int i = 0; i < NNUE_L2; i++)
            acc += l2[i] * w->w2[i][j];
        l3[j] = relu(acc);
    }

    /* L3 → output (linear) */
    float out = w->b3[0];
    for (int i = 0; i < NNUE_L3; i++)
        out += l3[i] * w->w3[i][0];

    /* scale: network outputs in "internal units"; map to centipawns  */
    return out * 400.0f;
}

/* ================================================================== */
/*  Weight utilities                                                   */
/* ================================================================== */
void nnue_copy_weights(NNUEWeights* dst, const NNUEWeights* src) {
    memcpy(dst, src, sizeof(NNUEWeights));
}

void nnue_apply_gradient(NNUEWeights* w, const NNUEWeights* grad, float lr) {
    /*  Simple SGD step: w -= lr * grad
        Both are flat arrays of float, so treat as float arrays.     */
    int n = (int)(sizeof(NNUEWeights) / sizeof(float));
    float* pw = (float*)w;
    const float* pg = (const float*)grad;
    for (int i = 0; i < n; i++) pw[i] -= lr * pg[i];
}
