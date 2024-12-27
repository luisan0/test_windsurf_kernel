/*
 * IMG Hash Accelerator Test Program
 * This is a standalone simulation of the IMG hardware hash accelerator
 * Author: Cascade AI
 * Date: 2024-12-27
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>

/* Register definitions */
#define CR_RESET                0x00
#define CR_MESSAGE_LENGTH_H     0x04
#define CR_MESSAGE_LENGTH_L     0x08
#define CR_CONTROL             0x0C
#define CR_INTSTAT             0x10
#define CR_INTENAB             0x14
#define CR_INTCLEAR            0x18
#define CR_RESULT_QUEUE        0x1C
#define CR_RSD0                0x40
#define CR_CORE_REV            0x50
#define CR_CORE_DES1           0x60
#define CR_CORE_DES2           0x70

/* Control register bit definitions */
#define CR_CONTROL_BYTE_ORDER_3210  0
#define CR_CONTROL_BYTE_ORDER_0123  1
#define CR_CONTROL_BYTE_ORDER_2310  2
#define CR_CONTROL_BYTE_ORDER_1032  3
#define CR_CONTROL_BYTE_ORDER_SHIFT 8

#define CR_CONTROL_ALGO_MD5    0
#define CR_CONTROL_ALGO_SHA1   1
#define CR_CONTROL_ALGO_SHA224 2
#define CR_CONTROL_ALGO_SHA256 3

/* Interrupt bits */
#define CR_INT_RESULTS_AVAILABLE     (1 << 0)
#define CR_INT_NEW_RESULTS_SET       (1 << 1)
#define CR_INT_RESULT_READ_ERR       (1 << 2)
#define CR_INT_MESSAGE_WRITE_ERROR   (1 << 3)
#define CR_INT_STATUS               (1 << 8)

/* Hash algorithm definitions */
#define HASH_MD5_DIGEST_SIZE    16
#define HASH_SHA1_DIGEST_SIZE   20
#define HASH_SHA224_DIGEST_SIZE 28
#define HASH_SHA256_DIGEST_SIZE 32

#define MAX_DIGEST_SIZE         HASH_SHA256_DIGEST_SIZE
#define MAX_BLOCK_SIZE         64
#define DMA_BUFFER_SIZE       4096

/* Driver flags */
#define FLAG_BUSY              (1 << 0)
#define FLAG_FINAL            (1 << 1)
#define FLAG_DMA_ACTIVE       (1 << 2)
#define FLAG_OUTPUT_READY     (1 << 3)
#define FLAG_INIT             (1 << 4)
#define FLAG_ERROR            (1 << 7)

/* Hash context structure */
struct hash_ctx {
    uint32_t flags;
    uint32_t algorithm;
    uint32_t digest_size;
    uint64_t total_bytes;
    uint8_t buffer[MAX_BLOCK_SIZE];
    size_t buffer_count;
    uint8_t digest[MAX_DIGEST_SIZE];
    pthread_mutex_t lock;
};

/* Hardware registers simulation */
struct img_hash_regs {
    uint32_t reset;
    uint32_t msg_length_h;
    uint32_t msg_length_l;
    uint32_t control;
    uint32_t intstat;
    uint32_t intenab;
    uint32_t intclear;
    uint32_t result_queue[8];  /* 8 words for SHA-256 */
    uint32_t core_rev;
    uint32_t core_des1;
    uint32_t core_des2;
};

/* Hardware device simulation */
struct img_hash_dev {
    struct img_hash_regs regs;
    struct hash_ctx *current_ctx;
    pthread_t processing_thread;
    bool thread_running;
    pthread_mutex_t dev_lock;
    pthread_cond_t processing_done;
};

/* Function declarations */
static void img_hash_init_regs(struct img_hash_dev *dev);
static int img_hash_process_data(struct img_hash_dev *dev, const uint8_t *data, size_t length);
static void img_hash_final_block(struct img_hash_dev *dev);
static void img_hash_get_digest(struct img_hash_dev *dev, uint8_t *out);
static void *img_hash_processing_thread(void *arg);

/* Initialize hardware registers */
static void img_hash_init_regs(struct img_hash_dev *dev) {
    memset(&dev->regs, 0, sizeof(dev->regs));
    dev->regs.core_rev = 0x01000000;  /* Version 1.0.0.0 */
    dev->regs.core_des1 = 0x12345678; /* Design info */
    dev->regs.core_des2 = 0x87654321; /* Additional design info */
}

/* Initialize hash context */
static struct hash_ctx *img_hash_init_ctx(uint32_t algorithm) {
    struct hash_ctx *ctx = calloc(1, sizeof(struct hash_ctx));
    if (!ctx)
        return NULL;

    ctx->algorithm = algorithm;
    switch (algorithm) {
    case CR_CONTROL_ALGO_MD5:
        ctx->digest_size = HASH_MD5_DIGEST_SIZE;
        break;
    case CR_CONTROL_ALGO_SHA1:
        ctx->digest_size = HASH_SHA1_DIGEST_SIZE;
        break;
    case CR_CONTROL_ALGO_SHA224:
        ctx->digest_size = HASH_SHA224_DIGEST_SIZE;
        break;
    case CR_CONTROL_ALGO_SHA256:
        ctx->digest_size = HASH_SHA256_DIGEST_SIZE;
        break;
    default:
        free(ctx);
        return NULL;
    }

    pthread_mutex_init(&ctx->lock, NULL);
    ctx->flags = FLAG_INIT;
    return ctx;
}

/* Initialize hardware device */
static struct img_hash_dev *img_hash_init_dev(void) {
    struct img_hash_dev *dev = calloc(1, sizeof(struct img_hash_dev));
    if (!dev)
        return NULL;

    img_hash_init_regs(dev);
    pthread_mutex_init(&dev->dev_lock, NULL);
    pthread_cond_init(&dev->processing_done, NULL);
    
    /* Start processing thread */
    dev->thread_running = true;
    if (pthread_create(&dev->processing_thread, NULL, img_hash_processing_thread, dev) != 0) {
        free(dev);
        return NULL;
    }

    return dev;
}

/* Cleanup functions */
static void img_hash_cleanup_ctx(struct hash_ctx *ctx) {
    if (!ctx)
        return;
    pthread_mutex_destroy(&ctx->lock);
    free(ctx);
}

static void img_hash_cleanup_dev(struct img_hash_dev *dev) {
    if (!dev)
        return;

    /* Stop processing thread */
    dev->thread_running = false;
    pthread_cond_signal(&dev->processing_done);
    pthread_join(dev->processing_thread, NULL);

    pthread_mutex_destroy(&dev->dev_lock);
    pthread_cond_destroy(&dev->processing_done);
    free(dev);
}

/* Process a block of data */
static int img_hash_process_data(struct img_hash_dev *dev, const uint8_t *data, size_t length) {
    struct hash_ctx *ctx = dev->current_ctx;
    if (!ctx)
        return -EINVAL;

    pthread_mutex_lock(&ctx->lock);

    /* Update total bytes processed */
    ctx->total_bytes += length;

    /* Set message length in hardware registers */
    dev->regs.msg_length_l = (uint32_t)(ctx->total_bytes & 0xFFFFFFFF);
    dev->regs.msg_length_h = (uint32_t)(ctx->total_bytes >> 32);

    /* Simulate hardware processing */
    usleep(100); /* Simulate processing delay */

    /* Set interrupt status */
    dev->regs.intstat |= CR_INT_RESULTS_AVAILABLE;

    pthread_mutex_unlock(&ctx->lock);
    return 0;
}

/* Process final block with padding */
static void img_hash_final_block(struct img_hash_dev *dev) {
    struct hash_ctx *ctx = dev->current_ctx;
    if (!ctx)
        return;

    pthread_mutex_lock(&ctx->lock);

    /* Set final block flag */
    ctx->flags |= FLAG_FINAL;

    /* Simulate final block processing */
    usleep(200); /* Longer delay for final block */

    /* Generate simulated digest based on algorithm */
    uint32_t *result = (uint32_t *)ctx->digest;
    for (size_t i = 0; i < ctx->digest_size / 4; i++) {
        /* Generate deterministic but different values for each algorithm */
        result[i] = 0xDEADBEEF ^ (ctx->algorithm << 24) ^ (i << 16) ^
                   (uint32_t)ctx->total_bytes;
        dev->regs.result_queue[i] = result[i];
    }

    /* Set completion status */
    ctx->flags |= FLAG_OUTPUT_READY;
    dev->regs.intstat |= CR_INT_NEW_RESULTS_SET;

    pthread_mutex_unlock(&ctx->lock);
}

/* Get the computed digest */
static void img_hash_get_digest(struct img_hash_dev *dev, uint8_t *out) {
    struct hash_ctx *ctx = dev->current_ctx;
    if (!ctx || !out)
        return;

    pthread_mutex_lock(&ctx->lock);
    memcpy(out, ctx->digest, ctx->digest_size);
    pthread_mutex_unlock(&ctx->lock);
}

/* Hardware processing thread */
static void *img_hash_processing_thread(void *arg) {
    struct img_hash_dev *dev = (struct img_hash_dev *)arg;
    
    while (dev->thread_running) {
        pthread_mutex_lock(&dev->dev_lock);
        
        /* Wait for work or shutdown signal */
        while (dev->thread_running && !(dev->regs.intstat & CR_INT_RESULTS_AVAILABLE)) {
            pthread_cond_wait(&dev->processing_done, &dev->dev_lock);
        }

        if (!dev->thread_running) {
            pthread_mutex_unlock(&dev->dev_lock);
            break;
        }

        /* Process any pending results */
        if (dev->regs.intstat & CR_INT_RESULTS_AVAILABLE) {
            /* Simulate hardware processing */
            usleep(500);
            
            /* Clear interrupt status */
            dev->regs.intstat &= ~CR_INT_RESULTS_AVAILABLE;
        }

        pthread_mutex_unlock(&dev->dev_lock);
    }

    return NULL;
}

/* Test functions */
static void print_digest(const char *label, const uint8_t *digest, size_t size) {
    printf("%s: ", label);
    for (size_t i = 0; i < size; i++)
        printf("%02x", digest[i]);
    printf("\n");
}

static void test_hash_operation(struct img_hash_dev *dev, uint32_t algorithm,
                              const char *algo_name, const char *test_data) {
    printf("\nTesting %s hash operation\n", algo_name);
    printf("Input data: %s\n", test_data);

    /* Initialize context */
    struct hash_ctx *ctx = img_hash_init_ctx(algorithm);
    if (!ctx) {
        printf("Failed to initialize %s context\n", algo_name);
        return;
    }

    /* Set as current context */
    dev->current_ctx = ctx;

    /* Process test data */
    size_t data_len = strlen(test_data);
    int ret = img_hash_process_data(dev, (const uint8_t *)test_data, data_len);
    if (ret != 0) {
        printf("Failed to process data for %s\n", algo_name);
        img_hash_cleanup_ctx(ctx);
        return;
    }

    /* Finalize hash operation */
    img_hash_final_block(dev);

    /* Get and print digest */
    uint8_t digest[MAX_DIGEST_SIZE];
    img_hash_get_digest(dev, digest);
    print_digest(algo_name, digest, ctx->digest_size);

    /* Cleanup */
    img_hash_cleanup_ctx(ctx);
    dev->current_ctx = NULL;
}

int main(void) {
    printf("IMG Hash Accelerator Test Program\n");
    printf("=================================\n");

    /* Initialize device */
    struct img_hash_dev *dev = img_hash_init_dev();
    if (!dev) {
        printf("Failed to initialize hash device\n");
        return 1;
    }

    /* Test data */
    const char *test_data = "The quick brown fox jumps over the lazy dog";

    /* Test all supported hash algorithms */
    test_hash_operation(dev, CR_CONTROL_ALGO_MD5, "MD5", test_data);
    test_hash_operation(dev, CR_CONTROL_ALGO_SHA1, "SHA1", test_data);
    test_hash_operation(dev, CR_CONTROL_ALGO_SHA224, "SHA224", test_data);
    test_hash_operation(dev, CR_CONTROL_ALGO_SHA256, "SHA256", test_data);

    /* Test with empty string */
    printf("\nTesting with empty string:\n");
    test_hash_operation(dev, CR_CONTROL_ALGO_SHA256, "SHA256", "");

    /* Test with long data */
    char long_data[1024];
    memset(long_data, 'A', sizeof(long_data) - 1);
    long_data[sizeof(long_data) - 1] = '\0';
    printf("\nTesting with long data (1023 'A' characters):\n");
    test_hash_operation(dev, CR_CONTROL_ALGO_SHA256, "SHA256", long_data);

    /* Cleanup device */
    img_hash_cleanup_dev(dev);

    printf("\nTest completed successfully!\n");
    return 0;
}
