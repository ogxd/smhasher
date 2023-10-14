#include <stdint.h>
#include <stdio.h>
#include <string.h>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif
#include "gxhash.h"

typedef int8x16_t state;

union ReinterpretUnion {
    int64x2_t int64;
    int32x4_t int32;
    uint32x4_t uint32;
    int8x16_t int8;
    uint8x16_t uint8;
};

static inline state create_empty() {
    return vdupq_n_s8(0);
}

static inline state load_unaligned(const state* p) {
    return vld1q_s8((const int8_t*)p);
}

static inline state get_partial(const state* p, int len) {
    static const int8_t MASK[32] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    int8x16_t mask = vld1q_s8(&MASK[16 - len]);
    int8x16_t partial = vandq_s8(load_unaligned(p), mask);
    partial = vaddq_u8(partial, vdupq_n_u8(len)); // Prevents padded zeroes to introduce bias
    return partial;
}

// static inline state get_partial(const state* data, int len) {
//     // Temporary buffer filled with zeros
//     uint8_t buffer[sizeof(state)] = {0};
//     // Copy data into the buffer
//     memcpy(buffer, data, len);
//     // Load the buffer into a int8x16_t vector
//     return vld1q_s8((const int8_t*)buffer);
// }

static inline uint8x16_t aes_encrypt(uint8x16_t data, uint8x16_t keys) {
    uint8x16_t encrypted = vaeseq_u8(data, vdupq_n_u8(0));
    uint8x16_t mixed = vaesmcq_u8(encrypted);
    return veorq_u8(mixed, keys);
}

static inline uint8x16_t aes_decrypt(uint8x16_t data, uint8x16_t keys) {
    uint8x16_t encrypted = vaesdq_u8(data, vdupq_n_u8(0));
    uint8x16_t mixed = vaesmcq_u8(encrypted);
    return veorq_u8(mixed, keys);
}

static inline uint8x16_t aes_encrypt_last(uint8x16_t data, uint8x16_t keys) {
    uint8x16_t encrypted = vaeseq_u8(data, vdupq_n_u8(0));
    return veorq_u8(encrypted, keys);
}

// Improvement idea : Hardcode a large buffer of integers to load vectors from for on-the-fly xoring

static inline state compress(state a, state b) {
    //static const uint32_t salt1_data[4] = {0x713B01D0, 0x8F2F35DB, 0xAF163956, 0x85459F85}; // Fail sparse
    static const int32_t salt1_data[4] = {3624366803, 3553132711, 2860740361, 2722013029}; // 
    uint32x4_t salt1 = vld1q_u32(salt1_data);
    // uint32x4_t salt2 = vld1q_u32(salt2_data);
    //b = veorq_s8(a, b);
    union ReinterpretUnion bu = { .int8 = b };
    union ReinterpretUnion au = { .int8 = a };
    union ReinterpretUnion result = { .uint8 = aes_encrypt(au.uint8, bu.uint8) };
    //state encrypted = result.int8;

    union ReinterpretUnion s = { .uint32 = salt1 };
    union ReinterpretUnion result2 = { .uint8 = aes_encrypt_last(result.uint8, bu.uint8) };
    
    state encrypted = result2.int8;

    return encrypted;
}

static inline uint64_t finalize(state hash, uint32_t seed) {
    static const uint32_t salt1_data[4] = {0x713B01D0, 0x8F2F35DB, 0xAF163956, 0x85459F85};
    static const uint32_t salt2_data[4] = {0x1DE09647, 0x92CFA39C, 0x3DD99ACA, 0xB89C054F};
    static const uint32_t salt3_data[4] = {0xC78B122B, 0x5544B1B7, 0x689D2B7D, 0xD0012E32};

    uint32x4_t salt0 = vdupq_n_u32(seed);
    uint32x4_t salt1 = vld1q_u32(salt1_data);
    uint32x4_t salt2 = vld1q_u32(salt2_data);
    uint32x4_t salt3 = vld1q_u32(salt3_data);

    union ReinterpretUnion hash_u = { .int8 = hash };
    hash_u.uint8 = aes_encrypt(hash_u.uint8, vreinterpretq_u8_u32(salt0));
    hash_u.uint8 = aes_encrypt(hash_u.uint8, vreinterpretq_u8_u32(salt1));
    hash_u.uint8 = aes_encrypt(hash_u.uint8, vreinterpretq_u8_u32(salt2));
    hash_u.uint8 = aes_encrypt_last(hash_u.uint8, vreinterpretq_u8_u32(salt3));

    return *(uint64_t*)&hash_u.int8;
}

uint64_t gxhash(const uint8_t* input, int len, uint32_t seed) {
    const int VECTOR_SIZE = sizeof(state);
    const state* p = (const state*)input;
    const state* v = p;
    const state* end_address;
    int remaining_blocks_count = len / VECTOR_SIZE;
    state hash_vector = create_empty();

    const int UNROLL_FACTOR = 8;
    if (len >= VECTOR_SIZE * UNROLL_FACTOR) {
        int unrollable_blocks_count = (len / (VECTOR_SIZE * UNROLL_FACTOR)) * UNROLL_FACTOR;
        end_address = v + unrollable_blocks_count;

        state s0 = load_unaligned(v++);
        state s1 = load_unaligned(v++);
        state s2 = load_unaligned(v++);
        state s3 = load_unaligned(v++);
        state s4 = load_unaligned(v++);
        state s5 = load_unaligned(v++);
        state s6 = load_unaligned(v++);
        state s7 = load_unaligned(v++);

        while (v < end_address) {
            state v0 = load_unaligned(v++);
            state v1 = load_unaligned(v++);
            state v2 = load_unaligned(v++);
            state v3 = load_unaligned(v++);
            state v4 = load_unaligned(v++);
            state v5 = load_unaligned(v++);
            state v6 = load_unaligned(v++);
            state v7 = load_unaligned(v++);

            s0 = compress(s0, v0);
            s1 = compress(s1, v1);
            s2 = compress(s2, v2);
            s3 = compress(s3, v3);
            s4 = compress(s4, v4);
            s5 = compress(s5, v5);
            s6 = compress(s6, v6);
            s7 = compress(s7, v7);
        }

        state a = compress(compress(s0, s1), compress(s2, s3));
        state b = compress(compress(s4, s5), compress(s6, s7));
        hash_vector = compress(a, b);

        remaining_blocks_count -= unrollable_blocks_count;
    }

    end_address = v + remaining_blocks_count;

    while (v < end_address) {
        state v0 = load_unaligned(v++);
        hash_vector = compress(hash_vector, v0);
    }

    int remaining_bytes = len % VECTOR_SIZE;
    if (remaining_bytes > 0) {
        state partial_vector = get_partial(v, remaining_bytes);
        hash_vector = compress(hash_vector, partial_vector);
    }

    return finalize(hash_vector, seed);
}
