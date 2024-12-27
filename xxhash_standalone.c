#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* Detect endianness */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define XXH_CPU_LITTLE_ENDIAN 1
#else
#define XXH_CPU_LITTLE_ENDIAN 0
#endif

/* Rotate functions */
#define xxh_rotl32(x, r) ((x << r) | (x >> (32 - r)))
#define xxh_rotl64(x, r) ((x << r) | (x >> (64 - r)))

/* Prime numbers used in hash calculations */
static const uint32_t PRIME32_1 = 2654435761U;
static const uint32_t PRIME32_2 = 2246822519U;
static const uint32_t PRIME32_3 = 3266489917U;
static const uint32_t PRIME32_4 =  668265263U;
static const uint32_t PRIME32_5 =  374761393U;

static const uint64_t PRIME64_1 = 11400714785074694791ULL;
static const uint64_t PRIME64_2 = 14029467366897019727ULL;
static const uint64_t PRIME64_3 =  1609587929392839161ULL;
static const uint64_t PRIME64_4 =  9650029242287828579ULL;
static const uint64_t PRIME64_5 =  2870177450012600261ULL;

/* Unaligned memory access */
static inline uint32_t get_unaligned_le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

static inline uint64_t get_unaligned_le64(const uint8_t *p)
{
    return (uint64_t)get_unaligned_le32(p) |
           ((uint64_t)get_unaligned_le32(p + 4) << 32);
}

/* State structures for streaming API */
struct xxh32_state {
    uint32_t total_len_32;
    uint32_t large_len;
    uint32_t v1;
    uint32_t v2;
    uint32_t v3;
    uint32_t v4;
    uint32_t mem32[4];
    uint32_t memsize;
};

struct xxh64_state {
    uint64_t total_len;
    uint64_t v1;
    uint64_t v2;
    uint64_t v3;
    uint64_t v4;
    uint64_t mem64[4];
    uint32_t memsize;
};

/* XXH32 functions */
static uint32_t xxh32_round(uint32_t seed, const uint32_t input)
{
    seed += input * PRIME32_2;
    seed = xxh_rotl32(seed, 13);
    seed *= PRIME32_1;
    return seed;
}

uint32_t xxh32(const void *input, size_t len, uint32_t seed)
{
    const uint8_t *p = (const uint8_t *)input;
    const uint8_t *b_end = p + len;
    uint32_t h32;

    if (len >= 16) {
        const uint8_t *const limit = b_end - 16;
        uint32_t v1 = seed + PRIME32_1 + PRIME32_2;
        uint32_t v2 = seed + PRIME32_2;
        uint32_t v3 = seed + 0;
        uint32_t v4 = seed - PRIME32_1;

        do {
            v1 = xxh32_round(v1, get_unaligned_le32(p));
            p += 4;
            v2 = xxh32_round(v2, get_unaligned_le32(p));
            p += 4;
            v3 = xxh32_round(v3, get_unaligned_le32(p));
            p += 4;
            v4 = xxh32_round(v4, get_unaligned_le32(p));
            p += 4;
        } while (p <= limit);

        h32 = xxh_rotl32(v1, 1) + xxh_rotl32(v2, 7) +
              xxh_rotl32(v3, 12) + xxh_rotl32(v4, 18);
    } else {
        h32 = seed + PRIME32_5;
    }

    h32 += (uint32_t)len;

    while (p + 4 <= b_end) {
        h32 += get_unaligned_le32(p) * PRIME32_3;
        h32 = xxh_rotl32(h32, 17) * PRIME32_4;
        p += 4;
    }

    while (p < b_end) {
        h32 += (*p) * PRIME32_5;
        h32 = xxh_rotl32(h32, 11) * PRIME32_1;
        p++;
    }

    h32 ^= h32 >> 15;
    h32 *= PRIME32_2;
    h32 ^= h32 >> 13;
    h32 *= PRIME32_3;
    h32 ^= h32 >> 16;

    return h32;
}

/* XXH64 functions */
static uint64_t xxh64_round(uint64_t acc, const uint64_t input)
{
    acc += input * PRIME64_2;
    acc = xxh_rotl64(acc, 31);
    acc *= PRIME64_1;
    return acc;
}

static uint64_t xxh64_merge_round(uint64_t acc, uint64_t val)
{
    val = xxh64_round(0, val);
    acc ^= val;
    acc = acc * PRIME64_1 + PRIME64_4;
    return acc;
}

uint64_t xxh64(const void *input, size_t len, uint64_t seed)
{
    const uint8_t *p = (const uint8_t *)input;
    const uint8_t *const b_end = p + len;
    uint64_t h64;

    if (len >= 32) {
        const uint8_t *const limit = b_end - 32;
        uint64_t v1 = seed + PRIME64_1 + PRIME64_2;
        uint64_t v2 = seed + PRIME64_2;
        uint64_t v3 = seed + 0;
        uint64_t v4 = seed - PRIME64_1;

        do {
            v1 = xxh64_round(v1, get_unaligned_le64(p));
            p += 8;
            v2 = xxh64_round(v2, get_unaligned_le64(p));
            p += 8;
            v3 = xxh64_round(v3, get_unaligned_le64(p));
            p += 8;
            v4 = xxh64_round(v4, get_unaligned_le64(p));
            p += 8;
        } while (p <= limit);

        h64 = xxh_rotl64(v1, 1) + xxh_rotl64(v2, 7) +
              xxh_rotl64(v3, 12) + xxh_rotl64(v4, 18);
        h64 = xxh64_merge_round(h64, v1);
        h64 = xxh64_merge_round(h64, v2);
        h64 = xxh64_merge_round(h64, v3);
        h64 = xxh64_merge_round(h64, v4);

    } else {
        h64 = seed + PRIME64_5;
    }

    h64 += (uint64_t)len;

    while (p + 8 <= b_end) {
        uint64_t k1 = xxh64_round(0, get_unaligned_le64(p));
        h64 ^= k1;
        h64 = xxh_rotl64(h64, 27) * PRIME64_1 + PRIME64_4;
        p += 8;
    }

    if (p + 4 <= b_end) {
        h64 ^= (uint64_t)(get_unaligned_le32(p)) * PRIME64_1;
        h64 = xxh_rotl64(h64, 23) * PRIME64_2 + PRIME64_3;
        p += 4;
    }

    while (p < b_end) {
        h64 ^= (*p) * PRIME64_5;
        h64 = xxh_rotl64(h64, 11) * PRIME64_1;
        p++;
    }

    h64 ^= h64 >> 33;
    h64 *= PRIME64_2;
    h64 ^= h64 >> 29;
    h64 *= PRIME64_3;
    h64 ^= h64 >> 32;

    return h64;
}

/* Test program */
void print_hash(const char *str)
{
    size_t len = strlen(str);
    uint32_t h32 = xxh32(str, len, 0);
    uint64_t h64 = xxh64(str, len, 0);
    
    printf("Input: \"%s\"\n", str);
    printf("XXH32: 0x%08x\n", h32);
    printf("XXH64: 0x%016llx\n\n", (unsigned long long)h64);
}

int main()
{
    printf("XXHash Test Program\n");
    printf("==================\n\n");

    /* Test different types of input */
    print_hash(""); // Empty string
    print_hash("Hello, World!"); // Simple string
    print_hash("The quick brown fox jumps over the lazy dog"); // Long string
    print_hash("abcdefghijklmnopqrstuvwxyz"); // Alphabet
    print_hash("12345678901234567890123456789012345678901234567890"); // Numbers

    /* Test with same content, different seeds */
    const char *test_str = "Test String";
    size_t len = strlen(test_str);
    printf("Same string with different seeds:\n");
    printf("String: \"%s\"\n", test_str);
    
    for (int i = 0; i < 5; i++) {
        uint32_t seed = i * 100;
        uint32_t h32 = xxh32(test_str, len, seed);
        uint64_t h64 = xxh64(test_str, len, seed);
        printf("Seed %d:\n", seed);
        printf("  XXH32: 0x%08x\n", h32);
        printf("  XXH64: 0x%016llx\n", (unsigned long long)h64);
    }

    return 0;
}
