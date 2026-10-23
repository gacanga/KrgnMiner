// eq_host.h - host side of Equihash(192,7) "kerrigan": BLAKE2b midstate, solution
// encoding, canonical ordering, full validation (independent of the GPU code) and
// a CPU emulation of the GPU solver (tests).  Apache-2.0.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include "../gpu/eq_common.h"

// BLAKE2b-384 state after the parameter block (personalization "kerrigan" || le32(192) || le32(7))
void eq_init_state(uint64_t h[8]);
// midstate of a 140-byte header (version .. nonce)
EqMidstate eq_midstate(const uint8_t header[EQ_HEADER_BYTES]);
// 48-byte BLAKE2b output of header || le32(g), computed from scratch (reference)
void eq_ref_hash(const uint8_t header[EQ_HEADER_BYTES], uint32_t g, uint8_t out[48]);

// 128 x 25-bit big-endian indices <-> 400 bytes (Zcash "minimal" encoding)
void eq_compress(const uint32_t idx[EQ_SOL_INDICES], uint8_t sol[EQ_SOL_BYTES]);
void eq_expand(const uint8_t sol[EQ_SOL_BYTES], uint32_t idx[EQ_SOL_INDICES]);

// true if the 128 indices are all different
bool eq_distinct(const uint32_t idx[EQ_SOL_INDICES]);
// canonical order: in every subtree the half starting with the smaller index first
void eq_canonical(uint32_t idx[EQ_SOL_INDICES]);
// full check of an ordered solution (collisions, order, distinct, final zero);
// why = reason on failure
bool eq_validate(const uint8_t header[EQ_HEADER_BYTES], const uint32_t idx[EQ_SOL_INDICES], std::string* why = nullptr);

// CPU emulation of the GPU solver (same buckets, slots, drops, tree order).
// Returns the candidate leaf lists (tree order, before any filtering).
struct EqCpuStats { uint32_t level_counts[EQ_K]; uint32_t dropped[EQ_K]; uint32_t ncands; };
std::vector<std::vector<uint32_t>> eq_cpu_solve(const EqMidstate& ms, EqCpuStats* st = nullptr);

// header || compactsize(400) || solution -> SHA256d (32 bytes, as stored: little-endian number)
void eq_block_hash(const uint8_t header[EQ_HEADER_BYTES], const uint8_t sol[EQ_SOL_BYTES], uint8_t out[32]);
