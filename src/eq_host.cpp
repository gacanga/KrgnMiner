// eq_host.cpp - see eq_host.h.  Apache-2.0.
#include "eq_host.h"
#include <string.h>
#include <stdlib.h>
#include <algorithm>
#include <openssl/sha.h>

static uint64_t ld64(const uint8_t* p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

void eq_init_state(uint64_t h[8])
{
    uint8_t P[64];
    memset(P, 0, sizeof P);
    P[0] = 48;          // digest length (512 / 192) * 192 / 8
    P[1] = 0;           // key length
    P[2] = 1;           // fanout
    P[3] = 1;           // depth
    memcpy(P + 48, "kerrigan", 8);
    uint32_t n = EQ_N, k = EQ_K;
    for (int i = 0; i < 4; i++) { P[56 + i] = (uint8_t)(n >> (8 * i)); P[60 + i] = (uint8_t)(k >> (8 * i)); }
    const uint64_t IV[8] = { EQ_B2_IV0, EQ_B2_IV1, EQ_B2_IV2, EQ_B2_IV3, EQ_B2_IV4, EQ_B2_IV5, EQ_B2_IV6, EQ_B2_IV7 };
    for (int i = 0; i < 8; i++) h[i] = IV[i] ^ ld64(P + 8 * i);
}

EqMidstate eq_midstate(const uint8_t header[EQ_HEADER_BYTES])
{
    EqMidstate ms;
    eq_init_state(ms.h);
    uint64_t m[16];
    for (int i = 0; i < 16; i++) m[i] = ld64(header + 8 * i);
    eq_blake2b_compress(ms.h, m, 128, 0);
    uint8_t t[16];
    memset(t, 0, sizeof t);
    memcpy(t, header + 128, 12);
    ms.tail[0] = ld64(t);
    ms.tail[1] = ld64(t + 8);
    return ms;
}

void eq_ref_hash(const uint8_t header[EQ_HEADER_BYTES], uint32_t g, uint8_t out[48])
{
    // straightforward two-block BLAKE2b, independent of the midstate path
    uint64_t h[8];
    eq_init_state(h);
    uint8_t buf[256];
    memset(buf, 0, sizeof buf);
    memcpy(buf, header, EQ_HEADER_BYTES);
    for (int i = 0; i < 4; i++) buf[EQ_HEADER_BYTES + i] = (uint8_t)(g >> (8 * i));
    uint64_t m[16];
    for (int i = 0; i < 16; i++) m[i] = ld64(buf + 8 * i);
    eq_blake2b_compress(h, m, 128, 0);
    for (int i = 0; i < 16; i++) m[i] = ld64(buf + 128 + 8 * i);
    eq_blake2b_compress(h, m, EQ_HEADER_BYTES + 4, 1);
    for (int i = 0; i < 6; i++)
        for (int b = 0; b < 8; b++) out[8 * i + b] = (uint8_t)(h[i] >> (8 * b));
}

void eq_compress(const uint32_t idx[EQ_SOL_INDICES], uint8_t sol[EQ_SOL_BYTES])
{
    memset(sol, 0, EQ_SOL_BYTES);
    size_t bit = 0;
    for (int i = 0; i < EQ_SOL_INDICES; i++)
        for (int b = EQ_INDEX_BITS - 1; b >= 0; b--, bit++)
            if ((idx[i] >> b) & 1) sol[bit >> 3] |= (uint8_t)(0x80u >> (bit & 7));
}

void eq_expand(const uint8_t sol[EQ_SOL_BYTES], uint32_t idx[EQ_SOL_INDICES])
{
    size_t bit = 0;
    for (int i = 0; i < EQ_SOL_INDICES; i++) {
        uint32_t v = 0;
        for (int b = 0; b < EQ_INDEX_BITS; b++, bit++) v = (v << 1) | ((sol[bit >> 3] >> (7 - (bit & 7))) & 1);
        idx[i] = v;
    }
}

bool eq_distinct(const uint32_t idx[EQ_SOL_INDICES])
{
    uint32_t s[EQ_SOL_INDICES];
    memcpy(s, idx, sizeof s);
    std::sort(s, s + EQ_SOL_INDICES);
    for (int i = 1; i < EQ_SOL_INDICES; i++)
        if (s[i] == s[i - 1]) return false;
    return true;
}

void eq_canonical(uint32_t idx[EQ_SOL_INDICES])
{
    uint32_t tmp[EQ_SOL_INDICES];
    for (int half = 1; half < EQ_SOL_INDICES; half <<= 1) {
        for (int base = 0; base < EQ_SOL_INDICES; base += 2 * half) {
            if (idx[base + half] < idx[base]) {
                memcpy(tmp, idx + base, half * sizeof(uint32_t));
                memmove(idx + base, idx + base + half, half * sizeof(uint32_t));
                memcpy(idx + base + half, tmp, half * sizeof(uint32_t));
            }
        }
    }
}

bool eq_validate(const uint8_t header[EQ_HEADER_BYTES], const uint32_t idx[EQ_SOL_INDICES], std::string* why)
{
    auto fail = [&](const char* w) { if (why) *why = w; return false; };
    for (int i = 0; i < EQ_SOL_INDICES; i++)
        if (idx[i] >= EQ_NSTRINGS) return fail("index out of range");
    if (!eq_distinct(idx)) return fail("repeated index");
    uint8_t X[EQ_SOL_INDICES][24];
    uint32_t first[EQ_SOL_INDICES];
    for (int i = 0; i < EQ_SOL_INDICES; i++) {
        uint8_t h[48];
        eq_ref_hash(header, idx[i] / 2, h);
        memcpy(X[i], h + (idx[i] % 2) * 24, 24);
        first[i] = idx[i];
    }
    int n = EQ_SOL_INDICES;
    for (int step = 0; step < EQ_K; step++) {
        int off = 3 * step;
        for (int i = 0; i < n; i += 2) {
            if (memcmp(X[i] + off, X[i + 1] + off, 3) != 0) return fail("no collision");
            if (first[i + 1] < first[i]) return fail("indices not ordered");
            for (int b = 0; b < 24; b++) X[i / 2][b] = X[i][b] ^ X[i + 1][b];
            first[i / 2] = first[i];
        }
        n /= 2;
    }
    for (int b = 3 * EQ_K; b < 24; b++)
        if (X[0][b]) return fail("final hash not zero");
    return true;
}

void eq_block_hash(const uint8_t header[EQ_HEADER_BYTES], const uint8_t sol[EQ_SOL_BYTES], uint8_t out[32])
{
    uint8_t buf[EQ_HEADER_BYTES + 3 + EQ_SOL_BYTES];
    memcpy(buf, header, EQ_HEADER_BYTES);
    buf[EQ_HEADER_BYTES] = 0xfd;
    buf[EQ_HEADER_BYTES + 1] = (uint8_t)(EQ_SOL_BYTES & 0xff);
    buf[EQ_HEADER_BYTES + 2] = (uint8_t)(EQ_SOL_BYTES >> 8);
    memcpy(buf + EQ_HEADER_BYTES + 3, sol, EQ_SOL_BYTES);
    uint8_t h1[32];
    SHA256(buf, sizeof buf, h1);
    SHA256(h1, 32, out);
}

// ---------------------------------------------------------------------------
// CPU emulation of the GPU solver
// ---------------------------------------------------------------------------
struct CpuShared {
    std::vector<uint32_t> heads, pairs;
    std::vector<uint16_t> next;
    uint32_t npairs = 0;
    EqShared S;
    CpuShared() : heads(EQ_NRESTS), pairs(EQ_MAX_PAIRS), next(EQ_NSLOTS)
    {
        S.data = nullptr; S.heads = heads.data(); S.next = next.data(); S.pairs = pairs.data(); S.npairs = &npairs;
    }
    void prepare(const EqMem& M, int lin, uint32_t b, uint32_t n)
    {
        for (auto& h : heads) h = EQ_NIL;
        npairs = 0;
        eq_phase_load(M, lin, b, S);
        eq_phase_link(lin, n, 0, 1, S);
#if EQ_PAIRLIST
        eq_phase_collect(n, 0, 1, S);
#endif
    }
};

template <int LIN>
static void cpu_step(const EqMem& M, CpuShared& sh)
{
    const uint32_t* cnt = M.counts + (size_t)LIN * EQ_NBUCKETS;
    for (uint32_t b = 0; b < EQ_NBUCKETS; b++) {
        uint32_t n = std::min(cnt[b], (uint32_t)EQ_NSLOTS);
#if EQ_FUSED
        // rounds of NT "threads": insert all, then walk all (same pairs as the GPU)
        const uint32_t NT = 512;
        for (auto& h : sh.heads) h = EQ_NIL;
        eq_phase_load(M, LIN, b, sh.S);
        for (uint32_t base = 0; base < n; base += NT) {
            const uint32_t e = std::min(n, base + NT);
            for (uint32_t s = base; s < e; s++) {
                uint32_t a[EQ_RW(LIN)];
                eq_load_rec<LIN>(sh.S.data + (size_t)s * EQ_RW(LIN), a);
                eq_fused_insert<LIN>(sh.S, s, a);
            }
            for (uint32_t s = base; s < e; s++) {
                uint32_t a[EQ_RW(LIN)];
                eq_load_rec<LIN>(sh.S.data + (size_t)s * EQ_RW(LIN), a);
                eq_fused_walk<LIN>(M, b, s, a, sh.S);
            }
        }
#else
        sh.prepare(M, LIN, b, n);
        if constexpr (LIN < 6) eq_phase_pairs<LIN>(M, b, n, 0, 1, sh.S);
        else eq_phase_final(M, b, n, 0, 1, sh.S);
#endif
    }
}

std::vector<std::vector<uint32_t>> eq_cpu_solve(const EqMidstate& ms, EqCpuStats* st)
{
    const size_t slots = (size_t)EQ_NBUCKETS * EQ_NSLOTS;
    EqMem M;
    std::vector<std::vector<uint32_t>> buf(EQ_K);
    for (int L = 0; L < EQ_K; L++) {
        if (L == 3) { M.rec[3] = buf[0].data(); continue; }   // level 3 reuses level 0, as on the GPU
        buf[L].assign(slots * EQ_RW(L), 0);
        M.rec[L] = buf[L].data();
    }
    std::vector<uint32_t> counts((size_t)EQ_K * EQ_NBUCKETS, 0);
    std::vector<EqTree> cands(EQ_MAX_CANDS);
    std::vector<uint32_t> sols((size_t)EQ_MAX_CANDS * EQ_SOL_INDICES);
    uint32_t ncands = 0;
    M.counts = counts.data();
    M.cands = cands.data();
    M.ncands = &ncands;
    M.sols = sols.data();

    for (uint32_t g = 0; g < EQ_NHASHES; g++) eq_phase_gen(M, ms, g);
    CpuShared sh;
    cpu_step<0>(M, sh);
    cpu_step<1>(M, sh);
    cpu_step<2>(M, sh);
    cpu_step<3>(M, sh);
    cpu_step<4>(M, sh);
    cpu_step<5>(M, sh);
    cpu_step<6>(M, sh);
    uint32_t nc = std::min(ncands, (uint32_t)EQ_MAX_CANDS);
    for (uint32_t c = 0; c < nc; c++)
        for (uint32_t t = 0; t < EQ_SOL_INDICES; t++) eq_phase_backtrack(M, c, t);
    if (st) {
        for (int L = 0; L < EQ_K; L++) {
            uint64_t tot = 0, drop = 0;
            for (uint32_t b = 0; b < EQ_NBUCKETS; b++) {
                uint32_t c = counts[(size_t)L * EQ_NBUCKETS + b];
                tot += c;
                if (c > EQ_NSLOTS) drop += c - EQ_NSLOTS;
            }
            st->level_counts[L] = (uint32_t)tot;
            st->dropped[L] = (uint32_t)drop;
        }
        st->ncands = ncands;
    }
    std::vector<std::vector<uint32_t>> out;
    for (uint32_t c = 0; c < nc; c++) out.emplace_back(sols.begin() + (size_t)c * EQ_SOL_INDICES, sols.begin() + (size_t)(c + 1) * EQ_SOL_INDICES);
    return out;
}
