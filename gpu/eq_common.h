// eq_common.h - Equihash(192,7) "kerrigan" solver core shared by the CUDA engine
// (kgpu.dll) and its CPU emulation used by the tests.  Apache-2.0.
//
// Wagner's algorithm, 7 collision steps on 24-bit digits:
//   level 0 : 2^25 strings of 24 bytes (2^24 BLAKE2b-384 hashes, 2 strings each)
//   step L  : (L = 1..6) pairs of level L-1 equal on digit L-1 (3 bytes) -> XOR,
//             the digit is dropped -> level L strings of 24 - 3L bytes
//   step 7  : pairs of level 6 equal on the last 6 bytes -> candidate solutions
// Each level is stored in NBUCKETS buckets (first BUCKBITS bits of the current
// digit) of NSLOTS slots.  Inside a bucket the remaining RESTBITS bits are matched
// with linked lists in shared memory.
//
// Records: on this GPU the cost of a scattered write is one 32-byte sector, however
// few bytes are written, so every string is ONE record holding its data and its
// tree entry (its two parents):
//   level 0 : 24 data bytes + leaf index                       -> 32 bytes
//   level 1 : 21 data bytes + the two leaf indices             -> 32 bytes
//   level 2 : 18 data bytes + (bucket, slot, slot) of level 1  -> 32 bytes
//   level 3..5 : 15 / 12 / 9 bytes + tree                      -> 24 bytes
//   level 6 : 6 data bytes + tree                              -> 16 bytes
// Levels 1..6 are kept until the end (their trees give the 128 leaves of a
// solution); level 3 reuses the memory of level 0.  About 160 bytes per slot
// (5.4 GB for 4096 x 8704 slots).  Measured on a P104-100: 24-byte records at
// levels 3..5 are slightly faster than 32-byte ones (less traffic).
//
// Compile-time options (all give the same solutions):
//   EQ_BUCKBITS     12 / 13  bucket bits (rest = 24 - BUCKBITS)
//   EQ_PAIRLIST     0 / 1    1 = pairs collected in a shared list then processed
//                            evenly, 0 = every thread walks its own lists
//   EQ_SHARE_CHECK  0 / 2    2 = at the final step, drop pairs whose two strings
//                            have a common parent (repeated indices), 0 = never
//   EQ_FUSED        0 / 1    1 = link and pairs fused: the block takes the bucket
//                            in rounds of one slot per thread; a thread inserts its
//                            slot, then (after one barrier) pairs it with all the
//                            earlier slots of its list, its record still in registers
//
// The phase functions below are written once and run either by CUDA threads
// (eq_gpu.cu) or by a sequential loop over (block, thread) (CPU emulation).
#pragma once
#include <stdint.h>

#ifdef __CUDACC__
#define EQ_HD __device__ __forceinline__
#define EQ_ATOMIC_ADD(p, v) atomicAdd((p), (v))
#define EQ_ATOMIC_EXCH(p, v) atomicExch((p), (v))
#else
#define EQ_HD static inline
template <typename T> static inline T eq_cpu_atomic_add(T* p, T v) { T o = *p; *p = o + v; return o; }
template <typename T> static inline T eq_cpu_atomic_exch(T* p, T v) { T o = *p; *p = v; return o; }
#define EQ_ATOMIC_ADD(p, v) eq_cpu_atomic_add((p), (v))
#define EQ_ATOMIC_EXCH(p, v) eq_cpu_atomic_exch((p), (v))
#endif

#define EQ_N 192
#define EQ_K 7
#define EQ_DIGIT_BITS 24                      // N / (K + 1)
#define EQ_INDEX_BITS 25                      // DIGIT_BITS + 1
#define EQ_NSTRINGS (1u << EQ_INDEX_BITS)     // 2^25 level-0 strings
#define EQ_NHASHES (EQ_NSTRINGS / 2)          // 2 strings per 48-byte BLAKE2b output
#define EQ_SOL_INDICES (1 << EQ_K)            // 128 indices per solution
#define EQ_SOL_BYTES 400                      // 128 x 25 bits
#define EQ_HEADER_BYTES 140                   // 108-byte header + 32-byte nonce

// ---------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------
#ifndef EQ_BUCKBITS
#define EQ_BUCKBITS 12
#endif
#define EQ_RESTBITS (EQ_DIGIT_BITS - EQ_BUCKBITS)
#define EQ_NBUCKETS (1u << EQ_BUCKBITS)
#define EQ_NRESTS (1u << EQ_RESTBITS)
#define EQ_RESTMASK (EQ_NRESTS - 1u)
// slots per bucket: average 2^(25 - BUCKBITS) + about 5.7 standard deviations
// (an overflow only drops the extra strings of that bucket)
#ifndef EQ_NSLOTS
#if EQ_BUCKBITS == 12
#define EQ_NSLOTS 8704u
#else
#define EQ_NSLOTS 4480u
#endif
#endif
#ifndef EQ_PAIRLIST
#define EQ_PAIRLIST 0
#endif
#ifndef EQ_SHARE_CHECK
#define EQ_SHARE_CHECK 2
#endif
#ifndef EQ_FUSED
#define EQ_FUSED (!EQ_PAIRLIST)
#endif
#define EQ_MAX_PAIRS (EQ_NSLOTS * 8u / 5u)    // pairs per bucket (average = strings per bucket)
#define EQ_NIL 0xffffu
#define EQ_MAX_CANDS 1024

#if EQ_NSLOTS > 65535 || EQ_BUCKBITS > 13 || EQ_BUCKBITS < 12
#error "unsupported Equihash layout"
#endif

// data words of a string at each level (24 - 3L bytes)
#define EQ_LW(level) ((24 - 3 * (level) + 3) / 4)            // 6 6 5 4 3 3 2
// record words (data + tree) at each level: 32 bytes up to level 2, 24 bytes for
// levels 3..5 (EQ_REC24 = 0: 32 bytes), 16 bytes at level 6
#ifndef EQ_REC24
#define EQ_REC24 1
#endif
#define EQ_RW(level) ((level) <= 2 ? 8 : ((level) <= 5 ? (EQ_REC24 ? 6 : 8) : 4))
// position of the tree words in the record
#define EQ_TOFF(level) EQ_LW(level)
EQ_HD int eq_level_words(int level) { return EQ_LW(level); }
#define EQ_MAX_WORDS 6

// current 24-bit digit (bytes 0..2 of the string, big-endian bit order; byte i of
// the string is byte i%4 of word i/4, little-endian)
EQ_HD uint32_t eq_digit_of(uint32_t w0)
{
    return ((w0 & 0xffu) << 16) | (w0 & 0xff00u) | ((w0 >> 16) & 0xffu);
}
EQ_HD uint32_t eq_bucket_of(uint32_t w0) { return eq_digit_of(w0) >> EQ_RESTBITS; }
EQ_HD uint32_t eq_rest_of(uint32_t w0) { return eq_digit_of(w0) & EQ_RESTMASK; }

EQ_HD uint32_t eq_shr24(uint32_t lo, uint32_t hi)    // bytes 3..6 of the 64-bit pair
{
#ifdef __CUDA_ARCH__
    return __funnelshift_r(lo, hi, 24);
#else
    return (lo >> 24) | (hi << 8);
#endif
}

EQ_HD uint32_t eq_ld(const uint32_t* p)
{
#ifdef __CUDA_ARCH__
    return __ldg(p);
#else
    return *p;
#endif
}

// whole record of level L (RW words) into r[]
template <int L>
EQ_HD void eq_load_rec(const uint32_t* p, uint32_t* r)
{
#ifdef __CUDA_ARCH__
    if constexpr (EQ_RW(L) == 8) {
        uint4 u = __ldg((const uint4*)p), v = __ldg((const uint4*)p + 1);
        r[0] = u.x; r[1] = u.y; r[2] = u.z; r[3] = u.w; r[4] = v.x; r[5] = v.y; r[6] = v.z; r[7] = v.w;
    } else if constexpr (EQ_RW(L) == 6) {
        uint2 a = __ldg((const uint2*)p), b = __ldg((const uint2*)p + 1), c = __ldg((const uint2*)p + 2);
        r[0] = a.x; r[1] = a.y; r[2] = b.x; r[3] = b.y; r[4] = c.x; r[5] = c.y;
    } else {
        uint4 u = __ldg((const uint4*)p);
        r[0] = u.x; r[1] = u.y; r[2] = u.z; r[3] = u.w;
    }
#else
    for (int i = 0; i < EQ_RW(L); i++) r[i] = p[i];
#endif
}

template <int L>
EQ_HD void eq_store_rec(uint32_t* d, const uint32_t* r)
{
#ifdef __CUDA_ARCH__
    if constexpr (EQ_RW(L) == 8) {
        ((uint4*)d)[0] = make_uint4(r[0], r[1], r[2], r[3]);
        ((uint4*)d)[1] = make_uint4(r[4], r[5], r[6], r[7]);
    } else if constexpr (EQ_RW(L) == 6) {
        ((uint2*)d)[0] = make_uint2(r[0], r[1]);
        ((uint2*)d)[1] = make_uint2(r[2], r[3]);
        ((uint2*)d)[2] = make_uint2(r[4], r[5]);
    } else {
        ((uint4*)d)[0] = make_uint4(r[0], r[1], r[2], r[3]);
    }
#else
    for (int i = 0; i < EQ_RW(L); i++) d[i] = r[i];
#endif
}

// XOR of two level-(L-1) strings without their first digit -> level-L data.
// Returns 0 if the result is all zero (the two strings were identical: such a
// pair can only lead to solutions with repeated indices).
template <int WIN, int WOUT>
EQ_HD int eq_xor_shift(const uint32_t* a, const uint32_t* b, uint32_t* out)
{
    uint32_t x[WIN + 1];
#pragma unroll
    for (int i = 0; i < WIN; i++) x[i] = a[i] ^ b[i];
    x[WIN] = 0;
    uint32_t any = 0;
#pragma unroll
    for (int i = 0; i < WOUT; i++) { out[i] = eq_shr24(x[i], x[i + 1]); any |= out[i]; }
    return any != 0;
}

// ---------------------------------------------------------------------------
// BLAKE2b (RFC 7693), 64-bit words, little-endian.  Only what Equihash needs:
// parameter block with digest length 48 and a 16-byte personalization.
// ---------------------------------------------------------------------------
#define EQ_B2_IV0 0x6a09e667f3bcc908ull
#define EQ_B2_IV1 0xbb67ae8584caa73bull
#define EQ_B2_IV2 0x3c6ef372fe94f82bull
#define EQ_B2_IV3 0xa54ff53a5f1d36f1ull
#define EQ_B2_IV4 0x510e527fade682d1ull
#define EQ_B2_IV5 0x9b05688c2b3e6c1full
#define EQ_B2_IV6 0x1f83d9abfb41bd6bull
#define EQ_B2_IV7 0x5be0cd19137e2179ull

EQ_HD uint64_t eq_rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

#define EQ_B2_G(a, b, c, d, x, y)            \
    do {                                     \
        a = a + b + (x);                     \
        d = eq_rotr64(d ^ a, 32);            \
        c = c + d;                           \
        b = eq_rotr64(b ^ c, 24);            \
        a = a + b + (y);                     \
        d = eq_rotr64(d ^ a, 16);            \
        c = c + d;                           \
        b = eq_rotr64(b ^ c, 63);            \
    } while (0)

// one compression; t = byte counter after this block, last = final block
EQ_HD void eq_blake2b_compress(uint64_t h[8], const uint64_t m[16], uint64_t t, int last)
{
    uint64_t v[16];
    for (int i = 0; i < 8; i++) v[i] = h[i];
    v[8] = EQ_B2_IV0; v[9] = EQ_B2_IV1; v[10] = EQ_B2_IV2; v[11] = EQ_B2_IV3;
    v[12] = EQ_B2_IV4 ^ t; v[13] = EQ_B2_IV5; v[14] = last ? ~EQ_B2_IV6 : EQ_B2_IV6; v[15] = EQ_B2_IV7;
#define EQ_B2_ROUND(s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11, s12, s13, s14, s15) \
    EQ_B2_G(v[0], v[4], v[8], v[12], m[s0], m[s1]);                                    \
    EQ_B2_G(v[1], v[5], v[9], v[13], m[s2], m[s3]);                                    \
    EQ_B2_G(v[2], v[6], v[10], v[14], m[s4], m[s5]);                                   \
    EQ_B2_G(v[3], v[7], v[11], v[15], m[s6], m[s7]);                                   \
    EQ_B2_G(v[0], v[5], v[10], v[15], m[s8], m[s9]);                                   \
    EQ_B2_G(v[1], v[6], v[11], v[12], m[s10], m[s11]);                                 \
    EQ_B2_G(v[2], v[7], v[8], v[13], m[s12], m[s13]);                                  \
    EQ_B2_G(v[3], v[4], v[9], v[14], m[s14], m[s15]);
    EQ_B2_ROUND(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15)
    EQ_B2_ROUND(14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3)
    EQ_B2_ROUND(11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4)
    EQ_B2_ROUND(7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8)
    EQ_B2_ROUND(9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13)
    EQ_B2_ROUND(2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9)
    EQ_B2_ROUND(12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11)
    EQ_B2_ROUND(13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10)
    EQ_B2_ROUND(6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5)
    EQ_B2_ROUND(10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0)
    EQ_B2_ROUND(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15)
    EQ_B2_ROUND(14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3)
#undef EQ_B2_ROUND
    for (int i = 0; i < 8; i++) h[i] ^= v[i] ^ v[i + 8];
}

// Midstate of an Equihash header: the BLAKE2b state after the first 128 bytes of
// the 140-byte header, and the 12 remaining bytes (nonce bytes 20..31).
struct EqMidstate {
    uint64_t h[8];
    uint64_t tail[2];       // tail bytes 0..7, bytes 8..11 in the low half of [1]
};

// Hash g (0 <= g < 2^24): BLAKE2b of header || le32(g) -> 6 words (48 bytes)
EQ_HD void eq_hash_index(const EqMidstate& ms, uint32_t g, uint64_t out[6])
{
    uint64_t h[8];
    for (int i = 0; i < 8; i++) h[i] = ms.h[i];
    uint64_t m[16];
    m[0] = ms.tail[0];
    m[1] = (ms.tail[1] & 0xffffffffull) | ((uint64_t)g << 32);
    for (int i = 2; i < 16; i++) m[i] = 0;
    eq_blake2b_compress(h, m, 144, 1);
    for (int i = 0; i < 6; i++) out[i] = h[i];
}

// ---------------------------------------------------------------------------
// Solver memory (device pointers on the GPU, host pointers in the emulation)
// ---------------------------------------------------------------------------
struct EqTree { uint32_t x, y; };            // x = bucket << 16 | slot0, y = slot1 (level 1: two leaf indices)

struct EqMem {
    uint32_t* rec[EQ_K];                      // records of level L (rec[3] may alias rec[0])
    uint32_t* counts;                         // (EQ_K) x NBUCKETS: slots used per bucket of level L
    EqTree* cands;                            // final pairs (level-6 slots)
    uint32_t* ncands;
    uint32_t* sols;                           // EQ_MAX_CANDS x 128 leaf indices (tree order)
};

EQ_HD uint32_t* eq_rec_ptr(const EqMem& M, int level, uint32_t bucket, uint32_t slot)
{
    return M.rec[level] + ((size_t)bucket * EQ_NSLOTS + slot) * EQ_RW(level);
}
EQ_HD EqTree eq_tree_of(const EqMem& M, int level, uint32_t bucket, uint32_t slot)
{
    const uint32_t* p = eq_rec_ptr(M, level, bucket, slot) + EQ_TOFF(level);
    EqTree t;
    t.x = eq_ld(p);
    t.y = eq_ld(p + 1);
    return t;
}

// Level 0: hash g gives strings 2g and 2g+1 (record: 6 data words + leaf index).
EQ_HD void eq_phase_gen(const EqMem& M, const EqMidstate& ms, uint32_t g)
{
    uint64_t hv[6];
    eq_hash_index(ms, g, hv);
    uint32_t w[12];
    for (int i = 0; i < 6; i++) { w[2 * i] = (uint32_t)hv[i]; w[2 * i + 1] = (uint32_t)(hv[i] >> 32); }
    for (int half = 0; half < 2; half++) {
        const uint32_t* s = w + 6 * half;
        uint32_t b = eq_bucket_of(s[0]);
        uint32_t slot = EQ_ATOMIC_ADD(&M.counts[b], 1u);
        if (slot >= EQ_NSLOTS) continue;
        uint32_t r[8] = { s[0], s[1], s[2], s[3], s[4], s[5], 2 * g + half, 0u };
        eq_store_rec<0>(eq_rec_ptr(M, 0, b, slot), r);
    }
}

// ---------------------------------------------------------------------------
// Collision steps.  One bucket of level L-1 per block:
//   link    lists by rest value (heads / next in shared memory)
//   collect (EQ_PAIRLIST: shared pair list)
//   pairs   XOR -> level L, or final -> candidates
// Between the phases the GPU block synchronizes; the CPU emulation runs each
// phase for all "threads" in turn.
// ---------------------------------------------------------------------------
struct EqShared {
    const uint32_t* data; // records of the bucket (global memory)
    uint32_t* heads;      // EQ_NRESTS, initialized to EQ_NIL
    uint16_t* next;       // EQ_NSLOTS
    uint32_t* pairs;      // EQ_MAX_PAIRS: j | s << 16  (EQ_PAIRLIST)
    uint32_t* npairs;     // 1 counter, initialized to 0
};

EQ_HD void eq_phase_load(const EqMem& M, int lin, uint32_t bucket, EqShared& S)
{
    S.data = M.rec[lin] + (size_t)bucket * EQ_NSLOTS * EQ_RW(lin);
}

EQ_HD void eq_phase_link(int lin, uint32_t n, uint32_t t, uint32_t nt, const EqShared& S)
{
    const int W = EQ_RW(lin);
    for (uint32_t s = t; s < n; s += nt) {
        uint32_t r = eq_rest_of(eq_ld(S.data + (size_t)s * W));
        S.next[s] = (uint16_t)EQ_ATOMIC_EXCH(&S.heads[r], s);
    }
}

EQ_HD void eq_phase_collect(uint32_t n, uint32_t t, uint32_t nt, const EqShared& S)
{
    for (uint32_t s = t; s < n; s += nt) {
        uint32_t j = S.next[s];
        while (j != EQ_NIL) {
            uint32_t k = EQ_ATOMIC_ADD(S.npairs, 1u);
            if (k < EQ_MAX_PAIRS) S.pairs[k] = j | (s << 16);
            j = S.next[j];
        }
    }
}

// one pair (j earlier, s later) of bucket `bucket` of level LIN; a = record of s
template <int LIN>
EQ_HD void eq_emit_pair(const EqMem& M, uint32_t bucket, uint32_t j, uint32_t s, const uint32_t* a, const EqShared& S)
{
    const int lout = LIN + 1;
    const int WIN = EQ_LW(LIN), WOUT = EQ_LW(LIN + 1);
    uint32_t b[EQ_RW(LIN)];
    eq_load_rec<LIN>(S.data + (size_t)j * EQ_RW(LIN), b);
    uint32_t r[EQ_RW(LIN + 1)];
    if (!eq_xor_shift<WIN, WOUT>(a, b, r)) return;
    uint32_t nb = eq_bucket_of(r[0]);
    uint32_t slot = EQ_ATOMIC_ADD(&M.counts[(size_t)lout * EQ_NBUCKETS + nb], 1u);
    if (slot >= EQ_NSLOTS) return;
    if (LIN == 0) {               // level 1 keeps the two leaf indices directly
        r[EQ_TOFF(1)] = b[EQ_TOFF(0)];
        r[EQ_TOFF(1) + 1] = a[EQ_TOFF(0)];
    } else {
        r[EQ_TOFF(LIN + 1)] = (bucket << 16) | j;
        r[EQ_TOFF(LIN + 1) + 1] = s;
    }
#pragma unroll
    for (int i = EQ_TOFF(LIN + 1) + 2; i < EQ_RW(LIN + 1); i++) r[i] = 0;
    eq_store_rec<LIN + 1>(eq_rec_ptr(M, lout, nb, slot), r);
}

template <int LIN>
EQ_HD void eq_phase_pairs(const EqMem& M, uint32_t bucket, uint32_t n, uint32_t t, uint32_t nt, const EqShared& S)
{
#if EQ_PAIRLIST
    (void)n;
    uint32_t np = *S.npairs;
    if (np > EQ_MAX_PAIRS) np = EQ_MAX_PAIRS;
    for (uint32_t k = t; k < np; k += nt) {
        uint32_t p = S.pairs[k], j = p & 0xffffu, s = p >> 16;
        uint32_t a[EQ_RW(LIN)];
        eq_load_rec<LIN>(S.data + (size_t)s * EQ_RW(LIN), a);
        eq_emit_pair<LIN>(M, bucket, j, s, a, S);
    }
#else
    for (uint32_t s = t; s < n; s += nt) {
        uint32_t j = S.next[s];
        if (j == EQ_NIL) continue;
        uint32_t a[EQ_RW(LIN)];
        eq_load_rec<LIN>(S.data + (size_t)s * EQ_RW(LIN), a);
        while (j != EQ_NIL) {
            eq_emit_pair<LIN>(M, bucket, j, s, a, S);
            j = S.next[j];
        }
    }
#endif
}

// true if two level-6 slots of the same bucket have a common parent: their pair
// could only give a solution with repeated indices.
EQ_HD bool eq_share_parent6(const uint32_t* a, const uint32_t* b)
{
#if EQ_SHARE_CHECK
    uint32_t ta = a[EQ_TOFF(6)], tb = b[EQ_TOFF(6)];
    if ((ta >> 16) != (tb >> 16)) return false;
    uint32_t a0 = ta & 0xffffu, a1 = a[EQ_TOFF(6) + 1], b0 = tb & 0xffffu, b1 = b[EQ_TOFF(6) + 1];
    return a0 == b0 || a0 == b1 || a1 == b0 || a1 == b1;
#else
    (void)a; (void)b;
    return false;
#endif
}

// Final step (level 6, 6 bytes): pairs equal on all 6 bytes -> candidates.
EQ_HD void eq_final_pair(const EqMem& M, uint32_t bucket, uint32_t j, uint32_t s, const EqShared& S)
{
    uint32_t a[4], b[4];
    eq_load_rec<6>(S.data + (size_t)s * 4, a);
    eq_load_rec<6>(S.data + (size_t)j * 4, b);
    if (a[0] != b[0] || a[1] != b[1]) return;
    if (eq_share_parent6(a, b)) return;
    uint32_t c = EQ_ATOMIC_ADD(M.ncands, 1u);
    if (c < EQ_MAX_CANDS) {
        EqTree tr;
        tr.x = (bucket << 16) | j;
        tr.y = s;
        M.cands[c] = tr;
    }
}

EQ_HD void eq_phase_final(const EqMem& M, uint32_t bucket, uint32_t n, uint32_t t, uint32_t nt, const EqShared& S)
{
#if EQ_PAIRLIST
    (void)n;
    uint32_t np = *S.npairs;
    if (np > EQ_MAX_PAIRS) np = EQ_MAX_PAIRS;
    for (uint32_t k = t; k < np; k += nt) {
        uint32_t p = S.pairs[k];
        eq_final_pair(M, bucket, p & 0xffffu, p >> 16, S);
    }
#else
    for (uint32_t s = t; s < n; s += nt)
        for (uint32_t j = S.next[s]; j != EQ_NIL; j = S.next[j]) eq_final_pair(M, bucket, j, s, S);
#endif
}

// Fused rounds (EQ_FUSED): insert slot s (record a), then walk its list.
template <int LIN>
EQ_HD void eq_fused_insert(const EqShared& S, uint32_t s, const uint32_t* a)
{
    S.next[s] = (uint16_t)EQ_ATOMIC_EXCH(&S.heads[eq_rest_of(a[0])], s);
}
template <int LIN>
EQ_HD void eq_fused_walk(const EqMem& M, uint32_t bucket, uint32_t s, const uint32_t* a, const EqShared& S)
{
    for (uint32_t j = S.next[s]; j != EQ_NIL; j = S.next[j]) {
        if constexpr (LIN < 6) eq_emit_pair<LIN>(M, bucket, j, s, a, S);
        else {
            uint32_t b[4];
            eq_load_rec<6>(S.data + (size_t)j * 4, b);
            if (a[0] != b[0] || a[1] != b[1] || eq_share_parent6(a, b)) continue;
            uint32_t c = EQ_ATOMIC_ADD(M.ncands, 1u);
            if (c < EQ_MAX_CANDS) {
                EqTree tr;
                tr.x = (bucket << 16) | j;
                tr.y = s;
                M.cands[c] = tr;
            }
        }
    }
}

// Leaf t (0..127, tree order) of candidate c.
EQ_HD void eq_phase_backtrack(const EqMem& M, uint32_t c, uint32_t t)
{
    EqTree tr = M.cands[c];
    uint32_t bucket = tr.x >> 16;
    uint32_t slot = ((t >> 6) & 1) ? tr.y : (tr.x & 0xffffu);
    for (int L = 6; L >= 2; L--) {
        EqTree e = eq_tree_of(M, L, bucket, slot);
        bucket = e.x >> 16;
        slot = ((t >> (L - 1)) & 1) ? e.y : (e.x & 0xffffu);
    }
    EqTree e = eq_tree_of(M, 1, bucket, slot);
    M.sols[c * EQ_SOL_INDICES + t] = (t & 1) ? e.y : e.x;
}
