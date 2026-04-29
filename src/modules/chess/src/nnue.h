#pragma once
/*  nnue.h — Efficiently Updatable Neural Network (NNUE) declaration.
    Architecture: 768 → 256 → 32 → 32 → 1
    CPU path: incremental accumulator updates.
    GPU path: full batch evaluation via CUDA kernels.              */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Network dimensions                                                 */
/* ------------------------------------------------------------------ */
#define NNUE_INPUT      768   /* HalfKP-style: 12 × 64              */
#define NNUE_L1         256   /* first hidden layer width             */
#define NNUE_L2          32   /* second hidden layer width            */
#define NNUE_L3          32   /* third hidden layer width             */
#define NNUE_OUTPUT       1

/* We keep two perspective accumulators (white POV, black POV)        */
#define NNUE_ACC_SIZE   (NNUE_L1 * 2)   /* 512 floats               */

/* ------------------------------------------------------------------ */
/*  Network weights (plain floats, aligned for SIMD/CUDA)             */
/* ------------------------------------------------------------------ */
#define NNUE_ALIGN 64   /* cache-line / AVX-512 alignment            */

typedef struct {
    /* Layer 0: input → L1 (two perspectives stacked)               */
    float  w0[NNUE_INPUT][NNUE_L1];   /* [768][256]                */
    float  b0[NNUE_L1];

    /* Layer 1: ACC (512) → L2                                      */
    float  w1[NNUE_L1 * 2][NNUE_L2]; /* [512][32]                 */
    float  b1[NNUE_L2];

    /* Layer 2: L2 → L3                                             */
    float  w2[NNUE_L2][NNUE_L3];     /* [32][32]                  */
    float  b2[NNUE_L3];

    /* Layer 3: L3 → output                                         */
    float  w3[NNUE_L3][NNUE_OUTPUT]; /* [32][1]                   */
    float  b3[NNUE_OUTPUT];
} NNUEWeights;

/* ------------------------------------------------------------------ */
/*  CPU accumulator (incremental updates)                              */
/* ------------------------------------------------------------------ */
typedef struct {
    float white[NNUE_L1];   /* accumulated L1 from white's POV      */
    float black[NNUE_L1];   /* accumulated L1 from black's POV      */
} NNUEAccumulator;

/* ------------------------------------------------------------------ */
/*  CPU-side network context                                           */
/* ------------------------------------------------------------------ */
typedef struct {
    NNUEWeights*     weights;  /* heap-allocated, NNUE_ALIGN aligned */
    NNUEAccumulator  acc;      /* current position accumulator       */
} NNUEContext;

/* ------------------------------------------------------------------ */
/*  CPU API                                                            */
/* ------------------------------------------------------------------ */
NNUEWeights* nnue_alloc_weights(void);
void         nnue_free_weights(NNUEWeights* w);
void         nnue_init_random(NNUEWeights* w, uint64_t seed);
bool         nnue_save(const NNUEWeights* w, const char* path);
bool         nnue_load(NNUEWeights* w, const char* path);

/*  Full refresh from feature vector (call after null/position reset) */
void  nnue_refresh(NNUEContext* ctx, const float* features);

/*  Incremental update: toggle one feature on or off                  */
void  nnue_update_feature(NNUEContext* ctx, int feature_idx,
                          int color_pov, bool add);

/*  Forward pass on CPU — returns centipawn evaluation (white-relative)*/
float nnue_evaluate(const NNUEContext* ctx, int side_to_move);

/*  Copy weights between two NNUEWeights structs                      */
void  nnue_copy_weights(NNUEWeights* dst, const NNUEWeights* src);

/*  Apply gradient (used by CPU trainer for benchmark model)          */
void  nnue_apply_gradient(NNUEWeights* w, const NNUEWeights* grad,
                          float lr);

/* ------------------------------------------------------------------ */
/*  GPU batch evaluation API (implemented in nnue.cu)                 */
/* ------------------------------------------------------------------ */

/*  Opaque GPU context                                                */
typedef struct NNUEGPUContext NNUEGPUContext;

NNUEGPUContext* nnue_gpu_init(const NNUEWeights* w,
                              int max_batch);  /* max positions/batch */
void            nnue_gpu_free(NNUEGPUContext* ctx);

/*  Push updated weights from host to GPU                             */
void  nnue_gpu_push_weights(NNUEGPUContext* ctx,
                            const NNUEWeights* w);

/*  Evaluate a batch of positions.
    features : [batch × NNUE_INPUT] row-major float array (host ptr)
    stm      : [batch] array of side-to-move values
    scores   : [batch] float output (host ptr, caller allocates)     */
void  nnue_gpu_evaluate_batch(NNUEGPUContext* ctx,
                              const float*   features,
                              const int*     stm,
                              float*         scores,
                              int            batch);

/*  Compute gradients for a batch (for self-play training).
    targets  : [batch] float — target evaluations from benchmark     */
void  nnue_gpu_backward_batch(NNUEGPUContext* ctx,
                              const float*   features,
                              const int*     stm,
                              const float*   targets,
                              float          lr,
                              int            batch);

/*  Pull updated weights from GPU back to host                        */
void  nnue_gpu_pull_weights(NNUEGPUContext* ctx, NNUEWeights* w);

#ifdef __cplusplus
}
#endif
