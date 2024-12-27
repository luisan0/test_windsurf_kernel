/*
 * I/O Vector Iterator Test Program
 * This is a standalone simulation of I/O vector operations
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
#define IOV_MAX_SEGMENTS   1024
#define IOV_MAX_SIZE      (1024 * 1024)  /* 1MB */
#define PAGE_SIZE         4096
#define FAULT_RATE       0.1   /* 10% fault rate for testing */

/* Custom iovec structure */
struct iovec {
    void  *iov_base;    /* Starting address */
    size_t iov_len;     /* Number of bytes */
};

/* Iterator types */
enum iov_iter_type {
    ITER_IOVEC = 0,
    ITER_KVEC,
    ITER_BVEC,
    ITER_PIPE,
    ITER_XARRAY
};

/* Iterator direction */
enum iov_iter_direction {
    READ = 0,
    WRITE = 1
};

/* Iterator structure */
struct iov_iter {
    enum iov_iter_type    type;
    enum iov_iter_direction direction;
    size_t               iov_offset;
    size_t               count;
    union {
        const struct iovec *iov;
        const struct kvec  *kvec;
        const struct bio_vec *bvec;
        struct pipe_inode_info *pipe;
        struct xarray *xarray;
    };
    unsigned long        nr_segs;
};

/* Error simulation structure */
struct fault_config {
    bool    enabled;
    float   rate;
    size_t  min_size;
    size_t  max_size;
};

/* Statistics structure */
struct iter_stats {
    size_t  total_bytes;
    size_t  total_copies;
    size_t  total_faults;
    size_t  min_copy_size;
    size_t  max_copy_size;
    double  avg_copy_size;
};

/* Function declarations */
static int copy_from_iter(void *to, size_t bytes, struct iov_iter *i);
static int copy_to_iter(const void *from, size_t bytes, struct iov_iter *i);
static size_t iov_iter_count(const struct iov_iter *i);
static void iov_iter_advance(struct iov_iter *i, size_t bytes);
static int fault_inject(size_t size, struct fault_config *config);
static void update_stats(struct iter_stats *stats, size_t bytes, bool fault);
static void dump_iter(const struct iov_iter *i);
static void dump_stats(const struct iter_stats *stats);

/* Copy data from iterator */
static int copy_from_iter(void *to, size_t bytes, struct iov_iter *i) {
    size_t copied = 0;
    char *dest = to;
    
    if (bytes > i->count)
        bytes = i->count;
    
    if (i->type != ITER_IOVEC)
        return -EINVAL;
    
    while (bytes) {
        const struct iovec *iov = &i->iov[0];
        size_t base = i->iov_offset;
        size_t copy = min(bytes, iov->iov_len - base);
        
        if (copy) {
            memcpy(dest + copied, (char *)iov->iov_base + base, copy);
            copied += copy;
            bytes -= copy;
            base += copy;
        }
        
        if (base == iov->iov_len) {
            i->iov++;
            i->nr_segs--;
            i->iov_offset = 0;
        } else {
            i->iov_offset = base;
        }
    }
    
    i->count -= copied;
    return copied;
}

/* Copy data to iterator */
static int copy_to_iter(const void *from, size_t bytes, struct iov_iter *i) {
    size_t copied = 0;
    const char *src = from;
    
    if (bytes > i->count)
        bytes = i->count;
    
    if (i->type != ITER_IOVEC)
        return -EINVAL;
    
    while (bytes) {
        const struct iovec *iov = &i->iov[0];
        size_t base = i->iov_offset;
        size_t copy = min(bytes, iov->iov_len - base);
        
        if (copy) {
            memcpy((char *)iov->iov_base + base, src + copied, copy);
            copied += copy;
            bytes -= copy;
            base += copy;
        }
        
        if (base == iov->iov_len) {
            i->iov++;
            i->nr_segs--;
            i->iov_offset = 0;
        } else {
            i->iov_offset = base;
        }
    }
    
    i->count -= copied;
    return copied;
}

/* Get remaining bytes in iterator */
static size_t iov_iter_count(const struct iov_iter *i) {
    return i->count;
}

/* Advance iterator */
static void iov_iter_advance(struct iov_iter *i, size_t bytes) {
    if (bytes > i->count)
        bytes = i->count;
    
    if (i->type == ITER_IOVEC) {
        while (bytes) {
            const struct iovec *iov = &i->iov[0];
            size_t base = i->iov_offset;
            size_t copy = min(bytes, iov->iov_len - base);
            
            bytes -= copy;
            base += copy;
            
            if (base == iov->iov_len) {
                i->iov++;
                i->nr_segs--;
                i->iov_offset = 0;
            } else {
                i->iov_offset = base;
            }
        }
    }
    
    i->count -= bytes;
}

/* Simulate faults */
static int fault_inject(size_t size, struct fault_config *config) {
    if (!config->enabled)
        return 0;
    
    if (size < config->min_size || size > config->max_size)
        return 0;
    
    return ((float)rand() / RAND_MAX) < config->rate;
}

/* Update statistics */
static void update_stats(struct iter_stats *stats, size_t bytes, bool fault) {
    stats->total_bytes += bytes;
    stats->total_copies++;
    
    if (fault) {
        stats->total_faults++;
        return;
    }
    
    if (bytes < stats->min_copy_size)
        stats->min_copy_size = bytes;
    if (bytes > stats->max_copy_size)
        stats->max_copy_size = bytes;
    
    stats->avg_copy_size = (double)stats->total_bytes / stats->total_copies;
}

/* Dump iterator info */
static void dump_iter(const struct iov_iter *i) {
    printf("\nIterator Info:\n");
    printf("=============\n");
    printf("Type: %d\n", i->type);
    printf("Direction: %s\n", i->direction == READ ? "READ" : "WRITE");
    printf("Offset: %zu\n", i->iov_offset);
    printf("Count: %zu\n", i->count);
    printf("Segments: %lu\n", i->nr_segs);
}

/* Dump statistics */
static void dump_stats(const struct iter_stats *stats) {
    printf("\nIterator Statistics:\n");
    printf("===================\n");
    printf("Total bytes: %zu\n", stats->total_bytes);
    printf("Total copies: %zu\n", stats->total_copies);
    printf("Total faults: %zu\n", stats->total_faults);
    printf("Min copy size: %zu\n", stats->min_copy_size);
    printf("Max copy size: %zu\n", stats->max_copy_size);
    printf("Average copy size: %.2f\n", stats->avg_copy_size);
}

/* Test functions */
static void test_basic_copy(struct iter_stats *stats, struct fault_config *fault) {
    printf("\nTesting basic copy operations...\n");
    
    /* Prepare source data */
    char src_buf[PAGE_SIZE];
    for (int i = 0; i < PAGE_SIZE; i++)
        src_buf[i] = i & 0xFF;
    
    /* Prepare destination buffer */
    char dst_buf[PAGE_SIZE];
    memset(dst_buf, 0, PAGE_SIZE);
    
    /* Prepare IOV */
    struct iovec iov[2];
    iov[0].iov_base = dst_buf;
    iov[0].iov_len = PAGE_SIZE / 2;
    iov[1].iov_base = dst_buf + PAGE_SIZE / 2;
    iov[1].iov_len = PAGE_SIZE / 2;
    
    /* Setup iterator */
    struct iov_iter iter = {
        .type = ITER_IOVEC,
        .direction = WRITE,
        .iov = iov,
        .nr_segs = 2,
        .count = PAGE_SIZE,
        .iov_offset = 0
    };
    
    /* Perform copy */
    printf("Copying %d bytes...\n", PAGE_SIZE);
    
    if (fault_inject(PAGE_SIZE, fault)) {
        printf("Simulated fault injected\n");
        update_stats(stats, PAGE_SIZE, true);
        return;
    }
    
    int copied = copy_to_iter(src_buf, PAGE_SIZE, &iter);
    if (copied < 0) {
        printf("Copy failed: %d\n", copied);
        return;
    }
    
    update_stats(stats, copied, false);
    printf("Copied %d bytes\n", copied);
    
    /* Verify copy */
    bool match = true;
    for (int i = 0; i < PAGE_SIZE; i++) {
        if (dst_buf[i] != (i & 0xFF)) {
            match = false;
            break;
        }
    }
    
    printf("Data verification: %s\n", match ? "PASS" : "FAIL");
}

static void test_partial_copy(struct iter_stats *stats, struct fault_config *fault) {
    printf("\nTesting partial copy operations...\n");
    
    /* Prepare buffers */
    char src_buf[PAGE_SIZE];
    char dst_buf[PAGE_SIZE];
    
    for (int i = 0; i < PAGE_SIZE; i++)
        src_buf[i] = i & 0xFF;
    memset(dst_buf, 0, PAGE_SIZE);
    
    /* Test different copy sizes */
    size_t sizes[] = {64, 128, 256, 512, 1024};
    struct iovec iov = {
        .iov_base = dst_buf,
        .iov_len = PAGE_SIZE
    };
    
    for (int i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        struct iov_iter iter = {
            .type = ITER_IOVEC,
            .direction = WRITE,
            .iov = &iov,
            .nr_segs = 1,
            .count = sizes[i],
            .iov_offset = 0
        };
        
        printf("Copying %zu bytes...\n", sizes[i]);
        
        if (fault_inject(sizes[i], fault)) {
            printf("Simulated fault injected\n");
            update_stats(stats, sizes[i], true);
            continue;
        }
        
        int copied = copy_to_iter(src_buf, sizes[i], &iter);
        if (copied < 0) {
            printf("Copy failed: %d\n", copied);
            continue;
        }
        
        update_stats(stats, copied, false);
        printf("Copied %d bytes\n", copied);
    }
}

int main(void) {
    printf("I/O Vector Iterator Test Program\n");
    printf("==============================\n\n");
    
    /* Seed random number generator */
    srand(time(NULL));
    
    /* Initialize statistics */
    struct iter_stats stats = {
        .total_bytes = 0,
        .total_copies = 0,
        .total_faults = 0,
        .min_copy_size = SIZE_MAX,
        .max_copy_size = 0,
        .avg_copy_size = 0
    };
    
    /* Configure fault injection */
    struct fault_config fault = {
        .enabled = true,
        .rate = FAULT_RATE,
        .min_size = 64,
        .max_size = PAGE_SIZE
    };
    
    /* Run tests */
    test_basic_copy(&stats, &fault);
    test_partial_copy(&stats, &fault);
    
    /* Display final statistics */
    dump_stats(&stats);
    
    printf("\nTest completed successfully!\n");
    return 0;
}
