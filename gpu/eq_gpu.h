// eq_gpu.h - C API of kgpu.dll (CUDA Equihash(192,7) "kerrigan" engine of KrgnMiner).
// Apache-2.0.  Plain C types only: the DLL is built with nvcc + MSVC, the miner with
// MinGW g++ (Windows) or g++ (Linux, linked directly).
//
// Usage per GPU: kg_create -> kg_solve (one nonce = one full Wagner run) -> kg_destroy.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KG_MAX_CANDS 1024      // = EQ_MAX_CANDS
#define KG_SOL_INDICES 128

int         kg_device_count(void);
// returns NULL on failure (see kg_last_error(NULL)).  tpb = threads per block of the
// collision kernels (multiple of 32, at most 1024, 0 = default 256).
void*       kg_create(int device, int tpb);
void        kg_destroy(void* ctx);
const char* kg_name(void* ctx);
const char* kg_last_error(void* ctx);
int         kg_compute_capability(void* ctx);
int         kg_sm_count(void* ctx);
uint64_t    kg_memory(void* ctx);          // total bytes of the card
uint64_t    kg_memory_used(void* ctx);     // bytes allocated by the solver
int         kg_set_tpb(void* ctx, int tpb);

// One Wagner run.  midstate = 10 words: BLAKE2b state h[0..7] after the first 128
// header bytes, then the 12 last header bytes (nonce 20..31) as 2 little-endian
// words.  On return sols holds *ncands x 128 leaf indices in tree order (not yet
// filtered / ordered), *ncands <= KG_MAX_CANDS.  level_counts (optional, 7 words) =
// strings produced at each level (before the slot limit), ms = GPU time.
int         kg_solve(void* ctx, const uint64_t* midstate, uint32_t* sols, int* ncands,
                     uint32_t* level_counts, double* ms);

// compile-time layout of this build (text)
const char* kg_layout(void);

// One run with a timing of each stage: stage_ms[0..8] = BLAKE2b, steps 1..6, final
// step, backtracking (n <= 9).
int         kg_profile(void* ctx, const uint64_t* midstate, double* stage_ms, int n);

#ifdef __cplusplus
}
#endif
