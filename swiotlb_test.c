/*
 * Software I/O TLB Test Program
 * This is a standalone simulation of SWIOTLB operations
 * Author: Cascade AI
 * Date: 2024-12-27
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

/* Constants */
#define PAGE_SIZE           4096
#define PAGE_SHIFT         12
#define PAGE_MASK          (~(PAGE_SIZE-1))
#define SWIOTLB_SIZE       (4 * 1024 * 1024)  /* 4MB default size */
#define SLOT_SIZE          128
#define MAX_SLOTS          (SWIOTLB_SIZE / SLOT_SIZE)
#define DMA_BIDIRECTIONAL  0
#define DMA_TO_DEVICE      1
#define DMA_FROM_DEVICE    2
#define DMA_NONE           3

/* Error codes */
#define SWIOTLB_OK        0
#define SWIOTLB_ENOMEM   -1
#define SWIOTLB_EBUSY    -2
#define SWIOTLB_EINVAL   -3

/* Flags */
#define SWIOTLB_VERBOSE   0x1
#define SWIOTLB_FORCE     0x2
#define SWIOTLB_COHERENT  0x4

/* Statistics counters */
struct swiotlb_stats {
    uint64_t allocs;
    uint64_t frees;
    uint64_t maps;
    uint64_t unmaps;
    uint64_t bounces;
    uint64_t sync_for_cpu;
    uint64_t sync_for_device;
    uint64_t errors;
};

/* Slot structure */
struct swiotlb_slot {
    void     *orig_addr;
    void     *buffer;
    size_t    size;
    int       direction;
    bool      used;
    uint32_t  flags;
};

/* SWIOTLB context */
struct swiotlb_context {
    void               *pool;
    struct swiotlb_slot *slots;
    size_t             pool_size;
    unsigned int       nr_slots;
    unsigned int       used_slots;
    uint32_t          flags;
    struct swiotlb_stats stats;
    bool              initialized;
};

/* Function declarations */
static struct swiotlb_context *swiotlb_init(size_t size);
static void swiotlb_cleanup(struct swiotlb_context *ctx);
static void *swiotlb_map(struct swiotlb_context *ctx, void *addr, size_t size, int direction);
static int swiotlb_unmap(struct swiotlb_context *ctx, void *dev_addr);
static int swiotlb_sync_for_cpu(struct swiotlb_context *ctx, void *dev_addr);
static int swiotlb_sync_for_device(struct swiotlb_context *ctx, void *dev_addr);
static struct swiotlb_slot *find_slot(struct swiotlb_context *ctx, void *addr);
static void dump_stats(const struct swiotlb_context *ctx);
static void hexdump(const void *data, size_t size);

/* Initialize SWIOTLB */
static struct swiotlb_context *swiotlb_init(size_t size) {
    struct swiotlb_context *ctx;
    
    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;
    
    /* Align size to page boundary */
    size = (size + PAGE_SIZE - 1) & PAGE_MASK;
    
    /* Allocate bounce buffer pool */
    ctx->pool = aligned_alloc(PAGE_SIZE, size);
    if (!ctx->pool) {
        free(ctx);
        return NULL;
    }
    
    /* Allocate slot tracking array */
    ctx->slots = calloc(MAX_SLOTS, sizeof(struct swiotlb_slot));
    if (!ctx->slots) {
        free(ctx->pool);
        free(ctx);
        return NULL;
    }
    
    ctx->pool_size = size;
    ctx->nr_slots = size / SLOT_SIZE;
    ctx->used_slots = 0;
    ctx->initialized = true;
    
    printf("SWIOTLB initialized with %zu bytes (%u slots)\n",
           size, ctx->nr_slots);
    
    return ctx;
}

/* Clean up SWIOTLB */
static void swiotlb_cleanup(struct swiotlb_context *ctx) {
    if (!ctx)
        return;
    
    if (ctx->used_slots > 0)
        printf("Warning: %u slots still in use during cleanup\n",
               ctx->used_slots);
    
    free(ctx->pool);
    free(ctx->slots);
    free(ctx);
}

/* Map address for DMA */
static void *swiotlb_map(struct swiotlb_context *ctx, void *addr,
                        size_t size, int direction) {
    unsigned int i;
    struct swiotlb_slot *slot = NULL;
    
    if (!ctx || !ctx->initialized)
        return NULL;
    
    if (size > SLOT_SIZE)
        return NULL;
    
    /* Find free slot */
    for (i = 0; i < ctx->nr_slots; i++) {
        if (!ctx->slots[i].used) {
            slot = &ctx->slots[i];
            break;
        }
    }
    
    if (!slot) {
        ctx->stats.errors++;
        return NULL;
    }
    
    /* Initialize slot */
    slot->orig_addr = addr;
    slot->buffer = ctx->pool + (i * SLOT_SIZE);
    slot->size = size;
    slot->direction = direction;
    slot->used = true;
    
    /* Copy data to bounce buffer if needed */
    if (direction != DMA_TO_DEVICE) {
        memcpy(slot->buffer, addr, size);
        ctx->stats.bounces++;
    }
    
    ctx->used_slots++;
    ctx->stats.maps++;
    
    return slot->buffer;
}

/* Unmap DMA address */
static int swiotlb_unmap(struct swiotlb_context *ctx, void *dev_addr) {
    struct swiotlb_slot *slot;
    
    if (!ctx || !ctx->initialized)
        return SWIOTLB_EINVAL;
    
    slot = find_slot(ctx, dev_addr);
    if (!slot)
        return SWIOTLB_EINVAL;
    
    /* Copy data back if needed */
    if (slot->direction != DMA_TO_DEVICE) {
        memcpy(slot->orig_addr, slot->buffer, slot->size);
        ctx->stats.bounces++;
    }
    
    /* Free slot */
    slot->used = false;
    ctx->used_slots--;
    ctx->stats.unmaps++;
    
    return SWIOTLB_OK;
}

/* Sync buffer for CPU access */
static int swiotlb_sync_for_cpu(struct swiotlb_context *ctx, void *dev_addr) {
    struct swiotlb_slot *slot;
    
    if (!ctx || !ctx->initialized)
        return SWIOTLB_EINVAL;
    
    slot = find_slot(ctx, dev_addr);
    if (!slot)
        return SWIOTLB_EINVAL;
    
    if (slot->direction == DMA_FROM_DEVICE) {
        memcpy(slot->orig_addr, slot->buffer, slot->size);
        ctx->stats.bounces++;
    }
    
    ctx->stats.sync_for_cpu++;
    return SWIOTLB_OK;
}

/* Sync buffer for device access */
static int swiotlb_sync_for_device(struct swiotlb_context *ctx, void *dev_addr) {
    struct swiotlb_slot *slot;
    
    if (!ctx || !ctx->initialized)
        return SWIOTLB_EINVAL;
    
    slot = find_slot(ctx, dev_addr);
    if (!slot)
        return SWIOTLB_EINVAL;
    
    if (slot->direction == DMA_TO_DEVICE) {
        memcpy(slot->buffer, slot->orig_addr, slot->size);
        ctx->stats.bounces++;
    }
    
    ctx->stats.sync_for_device++;
    return SWIOTLB_OK;
}

/* Find slot by device address */
static struct swiotlb_slot *find_slot(struct swiotlb_context *ctx, void *addr) {
    unsigned int i;
    
    for (i = 0; i < ctx->nr_slots; i++) {
        if (ctx->slots[i].used && ctx->slots[i].buffer == addr)
            return &ctx->slots[i];
    }
    
    return NULL;
}

/* Dump statistics */
static void dump_stats(const struct swiotlb_context *ctx) {
    printf("\nSWIOTLB Statistics:\n");
    printf("==================\n");
    printf("Total slots: %u\n", ctx->nr_slots);
    printf("Used slots: %u\n", ctx->used_slots);
    printf("Maps: %lu\n", ctx->stats.maps);
    printf("Unmaps: %lu\n", ctx->stats.unmaps);
    printf("Bounces: %lu\n", ctx->stats.bounces);
    printf("Sync for CPU: %lu\n", ctx->stats.sync_for_cpu);
    printf("Sync for device: %lu\n", ctx->stats.sync_for_device);
    printf("Errors: %lu\n", ctx->stats.errors);
}

/* Hex dump utility */
static void hexdump(const void *data, size_t size) {
    const unsigned char *buf = data;
    size_t i, j;
    
    for (i = 0; i < size; i += 16) {
        printf("%04zx: ", i);
        
        for (j = 0; j < 16; j++) {
            if (i + j < size)
                printf("%02x ", buf[i + j]);
            else
                printf("   ");
        }
        
        printf(" ");
        
        for (j = 0; j < 16; j++) {
            if (i + j < size) {
                if (buf[i + j] >= 32 && buf[i + j] <= 126)
                    printf("%c", buf[i + j]);
                else
                    printf(".");
            }
        }
        
        printf("\n");
    }
}

/* Test functions */
static void test_basic_mapping(struct swiotlb_context *ctx) {
    printf("\nTesting basic mapping...\n");
    
    /* Prepare test data */
    char src_buf[256];
    char verify_buf[256];
    
    for (int i = 0; i < sizeof(src_buf); i++)
        src_buf[i] = i & 0xFF;
    
    /* Map for device access */
    printf("Mapping buffer for device access...\n");
    void *dev_addr = swiotlb_map(ctx, src_buf, sizeof(src_buf), DMA_TO_DEVICE);
    if (!dev_addr) {
        printf("Mapping failed\n");
        return;
    }
    
    /* Simulate device operation */
    printf("Simulating device operation...\n");
    swiotlb_sync_for_device(ctx, dev_addr);
    
    /* Verify data */
    memcpy(verify_buf, dev_addr, sizeof(verify_buf));
    if (memcmp(src_buf, verify_buf, sizeof(src_buf)) == 0)
        printf("Data verification passed\n");
    else
        printf("Data verification failed\n");
    
    /* Unmap */
    printf("Unmapping buffer...\n");
    swiotlb_unmap(ctx, dev_addr);
}

static void test_bidirectional(struct swiotlb_context *ctx) {
    printf("\nTesting bidirectional mapping...\n");
    
    /* Prepare test data */
    char src_buf[256];
    char dst_buf[256];
    
    for (int i = 0; i < sizeof(src_buf); i++)
        src_buf[i] = i & 0xFF;
    
    memset(dst_buf, 0, sizeof(dst_buf));
    
    /* Map for bidirectional access */
    printf("Mapping buffer for bidirectional access...\n");
    void *dev_addr = swiotlb_map(ctx, src_buf, sizeof(src_buf),
                                DMA_BIDIRECTIONAL);
    if (!dev_addr) {
        printf("Mapping failed\n");
        return;
    }
    
    /* Simulate device read */
    printf("Simulating device read...\n");
    swiotlb_sync_for_device(ctx, dev_addr);
    
    /* Simulate device write */
    printf("Simulating device write...\n");
    for (int i = 0; i < sizeof(src_buf); i++)
        ((char *)dev_addr)[i] ^= 0xFF;
    
    /* Sync for CPU */
    printf("Syncing for CPU access...\n");
    swiotlb_sync_for_cpu(ctx, dev_addr);
    memcpy(dst_buf, src_buf, sizeof(dst_buf));
    
    /* Verify data */
    bool match = true;
    for (int i = 0; i < sizeof(src_buf); i++) {
        if (dst_buf[i] != (i & 0xFF)) {
            match = false;
            break;
        }
    }
    printf("Data verification: %s\n", match ? "PASS" : "FAIL");
    
    /* Unmap */
    printf("Unmapping buffer...\n");
    swiotlb_unmap(ctx, dev_addr);
}

int main(void) {
    printf("Software I/O TLB Test Program\n");
    printf("============================\n\n");
    
    /* Initialize SWIOTLB */
    struct swiotlb_context *ctx = swiotlb_init(SWIOTLB_SIZE);
    if (!ctx) {
        printf("Failed to initialize SWIOTLB\n");
        return 1;
    }
    
    /* Run tests */
    test_basic_mapping(ctx);
    test_bidirectional(ctx);
    
    /* Display statistics */
    dump_stats(ctx);
    
    /* Cleanup */
    swiotlb_cleanup(ctx);
    
    printf("\nTest completed successfully!\n");
    return 0;
}
