// eq_gpu.cu - CUDA Equihash(192,7) "kerrigan" engine of KrgnMiner (kgpu.dll on
// Windows, linked object on Linux).  Apache-2.0.
//
// One kg_solve = one nonce: 2^24 BLAKE2b -> 2^25 strings, 6 collision steps, the
// final step and the backtracking of the candidates.  The per-thread work is the
// code of eq_common.h (also run by the CPU emulation of the tests).
#include <cuda_runtime.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <chrono>
#include <vector>
#include "eq_common.h"
#include "eq_gpu.h"

// Windows exports come from windows/kgpu.def
#define KG_API

struct KgCtx {
    int device = -1;
    int tpb = 512;
    cudaDeviceProp prop;
    EqMem M;
    uint32_t* buf[EQ_K] = { nullptr };     // allocations (rec[3] = buf[0])
    uint64_t used = 0;
    cudaStream_t stream = nullptr;
    cudaEvent_t ev0 = nullptr, ev1 = nullptr;
    cudaEvent_t pev[12] = { nullptr };     // profiling events (kg_profile)
    bool profile = false;
    uint32_t* h_ncands = nullptr;          // pinned
    std::string err;
};

static std::string g_err;

static bool ck(KgCtx* c, cudaError_t e, const char* what)
{
    if (e == cudaSuccess) return true;
    char b[512];
    snprintf(b, sizeof b, "%s: %s", what, cudaGetErrorString(e));
    if (c) c->err = b; else g_err = b;
    return false;
}

// ---------------------------------------------------------------------------
// kernels
// ---------------------------------------------------------------------------
__global__ void k_gen(EqMem M, EqMidstate ms)
{
    uint32_t g = blockIdx.x * blockDim.x + threadIdx.x;
    if (g < EQ_NHASHES) eq_phase_gen(M, ms, g);
}

#if EQ_PAIRLIST
#define KG_PAIRS EQ_MAX_PAIRS
#else
#define KG_PAIRS 1
#endif

// one bucket per block: link, (collect), then pairs / final
template <int LIN>
__device__ __forceinline__ uint32_t k_prepare(const EqMem& M, uint32_t b, EqShared& S, uint32_t* heads, uint16_t* next,
                                              uint32_t* pairs, uint32_t* npairs)
{
    S.heads = heads; S.next = next; S.pairs = pairs; S.npairs = npairs;
    for (uint32_t r = threadIdx.x; r < EQ_NRESTS; r += blockDim.x) heads[r] = EQ_NIL;
    if (threadIdx.x == 0) *npairs = 0;
    uint32_t n = M.counts[(size_t)LIN * EQ_NBUCKETS + b];
    if (n > EQ_NSLOTS) n = EQ_NSLOTS;
    eq_phase_load(M, LIN, b, S);
    __syncthreads();
    eq_phase_link(LIN, n, threadIdx.x, blockDim.x, S);
    __syncthreads();
#if EQ_PAIRLIST
    eq_phase_collect(n, threadIdx.x, blockDim.x, S);
    __syncthreads();
#endif
    return n;
}

#if EQ_FUSED
// fused link + pairs: rounds of one slot per thread (see eq_common.h)
template <int LIN>
__global__ void __launch_bounds__(1024) k_step(EqMem M)
{
    __shared__ uint32_t heads[EQ_NRESTS];
    __shared__ uint16_t next[EQ_NSLOTS];
    EqShared S;
    S.heads = heads; S.next = next; S.pairs = nullptr; S.npairs = nullptr;
    const uint32_t b = blockIdx.x;
    for (uint32_t r = threadIdx.x; r < EQ_NRESTS; r += blockDim.x) heads[r] = EQ_NIL;
    uint32_t n = M.counts[(size_t)LIN * EQ_NBUCKETS + b];
    if (n > EQ_NSLOTS) n = EQ_NSLOTS;
    eq_phase_load(M, LIN, b, S);
    __syncthreads();
    for (uint32_t base = 0; base < n; base += blockDim.x) {
        const uint32_t s = base + threadIdx.x;
        const bool v = s < n;
        uint32_t a[EQ_RW(LIN)];
        if (v) {
            eq_load_rec<LIN>(S.data + (size_t)s * EQ_RW(LIN), a);
            eq_fused_insert<LIN>(S, s, a);
        }
        __syncthreads();
        if (v) eq_fused_walk<LIN>(M, b, s, a, S);
    }
}
#define k_final k_step<6>
#else
template <int LIN>
__global__ void __launch_bounds__(1024) k_step(EqMem M)
{
    __shared__ uint32_t heads[EQ_NRESTS];
    __shared__ uint16_t next[EQ_NSLOTS];
    __shared__ uint32_t pairs[KG_PAIRS];
    __shared__ uint32_t npairs;
    EqShared S;
    uint32_t n = k_prepare<LIN>(M, blockIdx.x, S, heads, next, pairs, &npairs);
    eq_phase_pairs<LIN>(M, blockIdx.x, n, threadIdx.x, blockDim.x, S);
}

__global__ void __launch_bounds__(1024) k_final(EqMem M)
{
    __shared__ uint32_t heads[EQ_NRESTS];
    __shared__ uint16_t next[EQ_NSLOTS];
    __shared__ uint32_t pairs[KG_PAIRS];
    __shared__ uint32_t npairs;
    EqShared S;
    uint32_t n = k_prepare<6>(M, blockIdx.x, S, heads, next, pairs, &npairs);
    eq_phase_final(M, blockIdx.x, n, threadIdx.x, blockDim.x, S);
}
#endif

__global__ void k_backtrack(EqMem M)
{
    uint32_t nc = *M.ncands;
    if (nc > EQ_MAX_CANDS) nc = EQ_MAX_CANDS;
    if (blockIdx.x >= nc) return;
    eq_phase_backtrack(M, blockIdx.x, threadIdx.x);
}

__global__ void k_level_totals(const uint32_t* counts, uint32_t* totals)
{
    // one block per level, 256 threads
    __shared__ uint32_t s[256];
    uint32_t sum = 0;
    for (uint32_t b = threadIdx.x; b < EQ_NBUCKETS; b += blockDim.x) sum += counts[(size_t)blockIdx.x * EQ_NBUCKETS + b];
    s[threadIdx.x] = sum;
    __syncthreads();
    for (int w = 128; w > 0; w >>= 1) {
        if ((int)threadIdx.x < w) s[threadIdx.x] += s[threadIdx.x + w];
        __syncthreads();
    }
    if (threadIdx.x == 0) totals[blockIdx.x] = s[0];
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
extern "C" {

KG_API int kg_device_count(void)
{
    int n = 0;
    if (cudaGetDeviceCount(&n) != cudaSuccess) return 0;
    return n;
}

KG_API const char* kg_last_error(void* ctx)
{
    return ctx ? ((KgCtx*)ctx)->err.c_str() : g_err.c_str();
}

KG_API void kg_destroy(void* ctx)
{
    KgCtx* c = (KgCtx*)ctx;
    if (!c) return;
    if (c->device >= 0) {
        cudaSetDevice(c->device);
        for (auto& b : c->buf) if (b) cudaFree(b);
        cudaFree(c->M.counts);
        cudaFree(c->M.cands);
        cudaFree(c->M.sols);
        if (c->h_ncands) cudaFreeHost(c->h_ncands);
        for (auto& e : c->pev) if (e) cudaEventDestroy(e);
        if (c->ev0) cudaEventDestroy(c->ev0);
        if (c->ev1) cudaEventDestroy(c->ev1);
        if (c->stream) cudaStreamDestroy(c->stream);
    }
    delete c;
}

KG_API void* kg_create(int device, int tpb)
{
    KgCtx* c = new KgCtx();
    memset(&c->M, 0, sizeof c->M);
    if (!ck(nullptr, cudaSetDevice(device), "cudaSetDevice")) { delete c; return nullptr; }
    c->device = device;
    if (!ck(nullptr, cudaGetDeviceProperties(&c->prop, device), "cudaGetDeviceProperties")) { kg_destroy(c); return nullptr; }
    if (tpb >= 32 && tpb <= 1024 && tpb % 32 == 0) c->tpb = tpb;
    const size_t slots = (size_t)EQ_NBUCKETS * EQ_NSLOTS;
    // one buffer per level, except level 3 which reuses level 0 (dead after step 1)
    for (int L = 0; L < EQ_K; L++) {
        if (L == 3) { c->M.rec[3] = c->buf[0]; continue; }
        const size_t bytes = slots * EQ_RW(L) * 4;
        char what[32];
        snprintf(what, sizeof what, "level %d records", L);
        if (!ck(nullptr, cudaMalloc((void**)&c->buf[L], bytes), what)) { kg_destroy(c); return nullptr; }
        c->M.rec[L] = c->buf[L];
        c->used += bytes;
    }
    struct A { void** p; size_t bytes; const char* what; } al[] = {
        { (void**)&c->M.counts, (size_t)EQ_K * EQ_NBUCKETS * 4 + 64, "counters" },
        { (void**)&c->M.cands, EQ_MAX_CANDS * sizeof(EqTree), "candidates" },
        { (void**)&c->M.sols, (size_t)EQ_MAX_CANDS * EQ_SOL_INDICES * 4, "solutions" },
    };
    for (auto& a : al) {
        if (!ck(nullptr, cudaMalloc(a.p, a.bytes), a.what)) { kg_destroy(c); return nullptr; }
        c->used += a.bytes;
    }
    // ncands lives right after the 7 x NBUCKETS counters: one memset clears both
    c->M.ncands = c->M.counts + (size_t)EQ_K * EQ_NBUCKETS;
    if (!ck(nullptr, cudaStreamCreateWithFlags(&c->stream, cudaStreamNonBlocking), "stream") ||
        !ck(nullptr, cudaEventCreate(&c->ev0), "event") || !ck(nullptr, cudaEventCreate(&c->ev1), "event") ||
        !ck(nullptr, cudaMallocHost((void**)&c->h_ncands, 64), "pinned")) {
        kg_destroy(c);
        return nullptr;
    }
    for (auto& e : c->pev) {
        if (!ck(nullptr, cudaEventCreate(&e), "event")) {
            kg_destroy(c);
            return nullptr;
        }
    }
    // the collision kernels only use shared memory for the lists: keep L1 large
    cudaFuncSetCacheConfig(k_step<0>, cudaFuncCachePreferL1);
    cudaFuncSetCacheConfig(k_step<1>, cudaFuncCachePreferL1);
    cudaFuncSetCacheConfig(k_step<2>, cudaFuncCachePreferL1);
    cudaFuncSetCacheConfig(k_step<3>, cudaFuncCachePreferL1);
    cudaFuncSetCacheConfig(k_step<4>, cudaFuncCachePreferL1);
    cudaFuncSetCacheConfig(k_step<5>, cudaFuncCachePreferL1);
    cudaFuncSetCacheConfig(k_final, cudaFuncCachePreferL1);
    return c;
}

KG_API const char* kg_name(void* ctx) { return ((KgCtx*)ctx)->prop.name; }
KG_API int kg_compute_capability(void* ctx) { KgCtx* c = (KgCtx*)ctx; return c->prop.major * 10 + c->prop.minor; }
KG_API int kg_sm_count(void* ctx) { return ((KgCtx*)ctx)->prop.multiProcessorCount; }
KG_API uint64_t kg_memory(void* ctx) { return ((KgCtx*)ctx)->prop.totalGlobalMem; }
KG_API uint64_t kg_memory_used(void* ctx) { return ((KgCtx*)ctx)->used; }
KG_API int kg_set_tpb(void* ctx, int tpb)
{
    KgCtx* c = (KgCtx*)ctx;
    if (tpb < 32 || tpb > 1024 || tpb % 32) return -1;
    c->tpb = tpb;
    return 0;
}

KG_API int kg_solve(void* ctx, const uint64_t* midstate, uint32_t* sols, int* ncands, uint32_t* level_counts, double* ms)
{
    KgCtx* c = (KgCtx*)ctx;
    cudaSetDevice(c->device);
    EqMidstate st;
    for (int i = 0; i < 8; i++) st.h[i] = midstate[i];
    st.tail[0] = midstate[8];
    st.tail[1] = midstate[9];
    cudaStream_t s = c->stream;
    const EqMem& M = c->M;
    const int tpb = c->tpb;
    const bool prof = c->profile;
#define KG_MARK(i) do { if (prof) cudaEventRecord(c->pev[i], s); } while (0)
    cudaEventRecord(c->ev0, s);
    KG_MARK(0);
    cudaMemsetAsync(M.counts, 0, (size_t)EQ_K * EQ_NBUCKETS * 4 + 64, s);
    k_gen<<<EQ_NHASHES / 256, 256, 0, s>>>(M, st);
    KG_MARK(1);
    k_step<0><<<EQ_NBUCKETS, tpb, 0, s>>>(M);
    KG_MARK(2);
    k_step<1><<<EQ_NBUCKETS, tpb, 0, s>>>(M);
    KG_MARK(3);
    k_step<2><<<EQ_NBUCKETS, tpb, 0, s>>>(M);
    KG_MARK(4);
    k_step<3><<<EQ_NBUCKETS, tpb, 0, s>>>(M);
    KG_MARK(5);
    k_step<4><<<EQ_NBUCKETS, tpb, 0, s>>>(M);
    KG_MARK(6);
    k_step<5><<<EQ_NBUCKETS, tpb, 0, s>>>(M);
    KG_MARK(7);
    k_final<<<EQ_NBUCKETS, tpb, 0, s>>>(M);
    KG_MARK(8);
    k_backtrack<<<EQ_MAX_CANDS, EQ_SOL_INDICES, 0, s>>>(M);
    KG_MARK(9);
#undef KG_MARK
    cudaMemcpyAsync(c->h_ncands, M.ncands, 4, cudaMemcpyDeviceToHost, s);
    if (level_counts) {
        // totals into the spare words after ncands (counts buffer has 64 extra bytes)
        k_level_totals<<<EQ_K, 256, 0, s>>>(M.counts, M.ncands + 1);
        cudaMemcpyAsync(c->h_ncands + 1, M.ncands + 1, EQ_K * 4, cudaMemcpyDeviceToHost, s);
    }
    cudaEventRecord(c->ev1, s);
    if (!ck(c, cudaGetLastError(), "kernel launch")) return -1;
    if (!ck(c, cudaStreamSynchronize(s), "solve")) return -1;
    uint32_t nc = c->h_ncands[0];
    if (nc > EQ_MAX_CANDS) nc = EQ_MAX_CANDS;
    if (nc && !ck(c, cudaMemcpy(sols, M.sols, (size_t)nc * EQ_SOL_INDICES * 4, cudaMemcpyDeviceToHost), "copy solutions"))
        return -1;
    *ncands = (int)nc;
    if (level_counts)
        for (int L = 0; L < EQ_K; L++) level_counts[L] = c->h_ncands[1 + L];
    if (ms) {
        float f = 0;
        cudaEventElapsedTime(&f, c->ev0, c->ev1);
        *ms = f;
    }
    return 0;
}

KG_API const char* kg_layout(void)
{
    static char b[160];
    snprintf(b, sizeof b, "%u buckets x %u slots, 32-byte records, %s, parent check %s", EQ_NBUCKETS, EQ_NSLOTS,
             EQ_FUSED ? "fused rounds" : (EQ_PAIRLIST ? "pair list" : "list walk"), EQ_SHARE_CHECK ? "final" : "off");
    return b;
}

KG_API int kg_profile(void* ctx, const uint64_t* midstate, double* stage_ms, int n)
{
    KgCtx* c = (KgCtx*)ctx;
    std::vector<uint32_t> sols((size_t)EQ_MAX_CANDS * EQ_SOL_INDICES);
    int nc = 0;
    double ms = 0;
    c->profile = true;
    int r = kg_solve(ctx, midstate, sols.data(), &nc, nullptr, &ms);
    c->profile = false;
    if (r) return r;
    for (int i = 0; i < n && i < 9; i++) {
        float f = 0;
        cudaEventElapsedTime(&f, c->pev[i], c->pev[i + 1]);
        stage_ms[i] = f;
    }
    return 0;
}

}  // extern "C"
