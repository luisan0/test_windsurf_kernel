/*
 * Generic Memory Allocator Test Program
 * This is a standalone simulation of the Linux kernel's genalloc.c functionality
 * Author: Cascade AI
 * Date: 2024-12-27
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>

/* Basic definitions to simulate Linux kernel environment */
#define BITS_PER_LONG 64
#define EXPORT_SYMBOL(sym)
#define EXPORT_SYMBOL_GPL(sym)
#define GFP_KERNEL 0
#define NUMA_NO_NODE -1
typedef uint64_t dma_addr_t;
typedef uint64_t phys_addr_t;
typedef int spinlock_t;
#define CONFIG_OF

/* Bitmap operations */
#define BITS_TO_LONGS(nr) (((nr) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define BIT_WORD(nr) ((nr) / BITS_PER_LONG)
#define BITMAP_FIRST_WORD_MASK(start) (~0UL << ((start) & (BITS_PER_LONG - 1)))
#define BITMAP_LAST_WORD_MASK(nbits) (~0UL >> (-(nbits) & (BITS_PER_LONG - 1)))

/* Atomic operations simulation */
#define READ_ONCE(x) (x)
#define cpu_relax() do {} while (0)

static inline bool try_cmpxchg(unsigned long *ptr, unsigned long *oldp, unsigned long new)
{
    unsigned long old = *oldp;
    if (*ptr == old) {
        *ptr = new;
        return true;
    }
    *oldp = *ptr;
    return false;
}

/* Forward declarations */
struct gen_pool;
struct gen_pool_chunk;
typedef unsigned long (*genpool_algo_t)(unsigned long *map,
    unsigned long size, unsigned long start, unsigned int nr, void *data,
    struct gen_pool *pool, unsigned long start_addr);

unsigned long gen_pool_first_fit(unsigned long *map, unsigned long size,
                               unsigned long start, unsigned int nr, void *data,
                               struct gen_pool *pool, unsigned long start_addr);

/* Main structures */
struct gen_pool_chunk {
    struct gen_pool_chunk *next_chunk;
    struct gen_pool *pool;
    unsigned long start_addr;
    unsigned long end_addr;
    unsigned long phys_addr;
    unsigned long *bits;
    void *owner;
    long avail;
};

struct gen_pool {
    struct gen_pool_chunk *chunks;
    int min_alloc_order;
    genpool_algo_t algo;
    void *data;
    const char *name;
    spinlock_t lock;
};

/* Spinlock simulation */
#define spin_lock_init(lock) (*(lock) = 0)
#define spin_lock(lock) do {} while (0)
#define spin_unlock(lock) do {} while (0)

/* Helper functions */
static inline size_t chunk_size(const struct gen_pool_chunk *chunk)
{
    return chunk->end_addr - chunk->start_addr + 1;
}

static inline int set_bits_ll(unsigned long *addr, unsigned long mask_to_set)
{
    unsigned long val = READ_ONCE(*addr);

    do {
        if (val & mask_to_set)
            return -EBUSY;
        cpu_relax();
    } while (!try_cmpxchg(addr, &val, val | mask_to_set));

    return 0;
}

static inline int clear_bits_ll(unsigned long *addr, unsigned long mask_to_clear)
{
    unsigned long val = READ_ONCE(*addr);

    do {
        if ((val & mask_to_clear) != mask_to_clear)
            return -EBUSY;
        cpu_relax();
    } while (!try_cmpxchg(addr, &val, val & ~mask_to_clear));

    return 0;
}

/* Bitmap operations */
static unsigned long bitmap_set_ll(unsigned long *map, unsigned long start, unsigned long nr)
{
    unsigned long *p = map + BIT_WORD(start);
    const unsigned long size = start + nr;
    int bits_to_set = BITS_PER_LONG - (start % BITS_PER_LONG);
    unsigned long mask_to_set = BITMAP_FIRST_WORD_MASK(start);

    while (nr >= bits_to_set) {
        if (set_bits_ll(p, mask_to_set))
            return nr;
        nr -= bits_to_set;
        bits_to_set = BITS_PER_LONG;
        mask_to_set = ~0UL;
        p++;
    }
    if (nr) {
        mask_to_set &= BITMAP_LAST_WORD_MASK(size);
        if (set_bits_ll(p, mask_to_set))
            return nr;
    }

    return 0;
}

static unsigned long bitmap_clear_ll(unsigned long *map, unsigned long start, unsigned long nr)
{
    unsigned long *p = map + BIT_WORD(start);
    const unsigned long size = start + nr;
    int bits_to_clear = BITS_PER_LONG - (start % BITS_PER_LONG);
    unsigned long mask_to_clear = BITMAP_FIRST_WORD_MASK(start);

    while (nr >= bits_to_clear) {
        if (clear_bits_ll(p, mask_to_clear))
            return nr;
        nr -= bits_to_clear;
        bits_to_clear = BITS_PER_LONG;
        mask_to_clear = ~0UL;
        p++;
    }
    if (nr) {
        mask_to_clear &= BITMAP_LAST_WORD_MASK(size);
        if (clear_bits_ll(p, mask_to_clear))
            return nr;
    }

    return 0;
}

/* Core functions */
struct gen_pool *gen_pool_create(int min_alloc_order, int nid)
{
    struct gen_pool *pool;

    pool = (struct gen_pool *)malloc(sizeof(struct gen_pool));
    if (pool != NULL) {
        spin_lock_init(&pool->lock);
        pool->chunks = NULL;
        pool->min_alloc_order = min_alloc_order;
        pool->algo = gen_pool_first_fit;
        pool->data = NULL;
        pool->name = NULL;
    }
    return pool;
}

int gen_pool_add_virt(struct gen_pool *pool, unsigned long virt, phys_addr_t phys,
                     size_t size, int nid)
{
    struct gen_pool_chunk *chunk;
    unsigned long nbits = size >> pool->min_alloc_order;
    unsigned long nbytes = sizeof(struct gen_pool_chunk) +
                          BITS_TO_LONGS(nbits) * sizeof(long);

    chunk = (struct gen_pool_chunk *)calloc(1, nbytes);
    if (chunk == NULL)
        return -ENOMEM;

    chunk->phys_addr = phys;
    chunk->start_addr = virt;
    chunk->end_addr = virt + size - 1;
    chunk->owner = NULL;
    chunk->avail = size;
    chunk->bits = (unsigned long *)(chunk + 1);
    memset(chunk->bits, 0, BITS_TO_LONGS(nbits) * sizeof(long));

    spin_lock(&pool->lock);
    chunk->next_chunk = pool->chunks;
    pool->chunks = chunk;
    spin_unlock(&pool->lock);

    return 0;
}

/* First-fit allocation algorithm */
unsigned long gen_pool_first_fit(unsigned long *map, unsigned long size,
                               unsigned long start, unsigned int nr, void *data,
                               struct gen_pool *pool, unsigned long start_addr)
{
    unsigned long start_bit = 0;
    unsigned long len = size;
    unsigned long index;
    unsigned long i;

    for (i = start; i < len; i++) {
        index = i;
        start_bit = i;

        while (index < len && !(map[index / BITS_PER_LONG] &
               (1UL << (index % BITS_PER_LONG)))) {
            index++;
            if (index - start_bit == nr)
                return start_bit;
        }
        i = index;
    }
    return -ENOMEM;
}

/* Memory allocation */
unsigned long gen_pool_alloc(struct gen_pool *pool, size_t size)
{
    struct gen_pool_chunk *chunk;
    unsigned long addr = 0;
    unsigned long flags;
    int order = pool->min_alloc_order;
    int nbits = size >> order;
    int start_bit = 0, remain;

    if (size < (1UL << order))
        return 0;

    spin_lock(&pool->lock);
    chunk = pool->chunks;
    while (chunk != NULL) {
        if (chunk->avail >= size) {
            remain = bitmap_set_ll(chunk->bits, start_bit, nbits);
            if (remain == 0) {
                addr = chunk->start_addr + ((unsigned long)start_bit << order);
                chunk->avail -= size;
                break;
            }
        }
        chunk = chunk->next_chunk;
    }
    spin_unlock(&pool->lock);

    return addr;
}

/* Memory deallocation */
void gen_pool_free(struct gen_pool *pool, unsigned long addr, size_t size)
{
    struct gen_pool_chunk *chunk;
    int order = pool->min_alloc_order;
    int start_bit, nbits;

    nbits = size >> order;
    spin_lock(&pool->lock);
    chunk = pool->chunks;
    while (chunk != NULL) {
        if (addr >= chunk->start_addr && addr <= chunk->end_addr) {
            start_bit = (addr - chunk->start_addr) >> order;
            bitmap_clear_ll(chunk->bits, start_bit, nbits);
            chunk->avail += size;
            break;
        }
        chunk = chunk->next_chunk;
    }
    spin_unlock(&pool->lock);
}

/* Pool destruction */
void gen_pool_destroy(struct gen_pool *pool)
{
    struct gen_pool_chunk *chunk = pool->chunks;
    struct gen_pool_chunk *next_chunk;

    while (chunk != NULL) {
        next_chunk = chunk->next_chunk;
        free(chunk);
        chunk = next_chunk;
    }
    free(pool);
}

/* Memory status functions */
size_t gen_pool_avail(struct gen_pool *pool)
{
    struct gen_pool_chunk *chunk;
    size_t avail = 0;

    spin_lock(&pool->lock);
    chunk = pool->chunks;
    while (chunk != NULL) {
        avail += chunk->avail;
        chunk = chunk->next_chunk;
    }
    spin_unlock(&pool->lock);
    return avail;
}

size_t gen_pool_size(struct gen_pool *pool)
{
    struct gen_pool_chunk *chunk;
    size_t size = 0;

    spin_lock(&pool->lock);
    chunk = pool->chunks;
    while (chunk != NULL) {
        size += chunk_size(chunk);
        chunk = chunk->next_chunk;
    }
    spin_unlock(&pool->lock);
    return size;
}

/* Test scenario */
void print_memory_status(struct gen_pool *pool, const char *message)
{
    printf("\n=== %s ===\n", message);
    printf("Total pool size: %zu bytes\n", gen_pool_size(pool));
    printf("Available memory: %zu bytes\n", gen_pool_avail(pool));
    printf("Used memory: %zu bytes\n", gen_pool_size(pool) - gen_pool_avail(pool));
}

#define TEST_POOL_SIZE (1024 * 1024)  // 1MB
#define MIN_ALLOC_ORDER 12            // 4KB minimum allocation
#define NUM_ALLOCATIONS 10
#define MAX_ALLOC_SIZE (64 * 1024)    // 64KB maximum allocation

int main()
{
    struct gen_pool *pool;
    unsigned long addrs[NUM_ALLOCATIONS];
    size_t sizes[NUM_ALLOCATIONS];
    int i;
    
    // Seed random number generator
    srand(time(NULL));

    // Create memory pool
    pool = gen_pool_create(MIN_ALLOC_ORDER, NUMA_NO_NODE);
    if (!pool) {
        printf("Failed to create memory pool\n");
        return -1;
    }

    // Add virtual memory region to pool
    if (gen_pool_add_virt(pool, 0x100000000ULL, 0, TEST_POOL_SIZE, NUMA_NO_NODE)) {
        printf("Failed to add memory to pool\n");
        gen_pool_destroy(pool);
        return -1;
    }

    print_memory_status(pool, "Initial pool status");

    // Perform random allocations
    printf("\nPerforming random allocations...\n");
    for (i = 0; i < NUM_ALLOCATIONS; i++) {
        // Generate random size aligned to MIN_ALLOC_ORDER
        sizes[i] = (rand() % (MAX_ALLOC_SIZE >> MIN_ALLOC_ORDER) + 1) << MIN_ALLOC_ORDER;
        
        addrs[i] = gen_pool_alloc(pool, sizes[i]);
        if (!addrs[i]) {
            printf("Allocation %d failed (requested %zu bytes)\n", i, sizes[i]);
            continue;
        }
        printf("Allocation %d: %zu bytes at address 0x%lx\n", i, sizes[i], addrs[i]);
    }

    print_memory_status(pool, "After allocations");

    // Free half of the allocations
    printf("\nFreeing half of the allocations...\n");
    for (i = 0; i < NUM_ALLOCATIONS / 2; i++) {
        if (addrs[i]) {
            gen_pool_free(pool, addrs[i], sizes[i]);
            printf("Freed allocation %d: %zu bytes at address 0x%lx\n", i, sizes[i], addrs[i]);
        }
    }

    print_memory_status(pool, "After partial free");

    // Try to allocate a large chunk
    printf("\nTrying to allocate a large chunk...\n");
    size_t large_size = TEST_POOL_SIZE / 2;
    unsigned long large_addr = gen_pool_alloc(pool, large_size);
    if (large_addr) {
        printf("Large allocation successful: %zu bytes at address 0x%lx\n", large_size, large_addr);
        gen_pool_free(pool, large_addr, large_size);
    } else {
        printf("Large allocation failed (requested %zu bytes)\n", large_size);
    }

    print_memory_status(pool, "After large allocation test");

    // Free remaining allocations
    printf("\nFreeing remaining allocations...\n");
    for (i = NUM_ALLOCATIONS / 2; i < NUM_ALLOCATIONS; i++) {
        if (addrs[i]) {
            gen_pool_free(pool, addrs[i], sizes[i]);
            printf("Freed allocation %d: %zu bytes at address 0x%lx\n", i, sizes[i], addrs[i]);
        }
    }

    print_memory_status(pool, "Final pool status");

    // Cleanup
    gen_pool_destroy(pool);
    printf("\nMemory pool destroyed\n");

    return 0;
}
