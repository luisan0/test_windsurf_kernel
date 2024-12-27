/*
 * Block I/O Merge Test Program
 * This is a standalone simulation of the Linux kernel's blk-merge.c functionality
 * Author: Cascade AI
 * Date: 2024-12-27
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <assert.h>

/* Basic definitions */
#define SECTOR_SHIFT 9
#define SECTOR_SIZE (1 << SECTOR_SHIFT)
#define PAGE_SIZE 4096
#define BIO_MAX_PAGES 256
#define QUEUE_MAX_SEGMENTS 128
#define MAX_PHYS_SEGMENTS 128
#define UINT_MAX 0xffffffff

typedef uint64_t sector_t;
typedef uint64_t phys_addr_t;

/* Flags for bio and request operations */
#define REQ_OP_MASK 0xff
#define REQ_OP_READ 0
#define REQ_OP_WRITE 1
#define REQ_OP_DISCARD 4
#define REQ_OP_WRITE_ZEROES 9
#define REQ_ATOMIC (1ULL << 8)
#define REQ_FAILFAST_DEV (1ULL << 9)
#define REQ_FAILFAST_TRANSPORT (1ULL << 10)
#define REQ_FAILFAST_DRIVER (1ULL << 11)

/* Queue limits structure */
struct queue_limits {
    unsigned int max_segments;
    unsigned int max_sectors;
    unsigned int max_segment_size;
    unsigned int physical_block_size;
    unsigned int logical_block_size;
    unsigned int io_min;
    unsigned int io_opt;
    unsigned int max_discard_sectors;
    unsigned int max_write_zeroes_sectors;
    unsigned int discard_granularity;
    unsigned int discard_alignment;
    unsigned int chunk_sectors;
    unsigned int atomic_write_max_sectors;
    unsigned int atomic_write_boundary_sectors;
};

/* Bio vector structure */
struct bio_vec {
    void *bv_page;
    unsigned int bv_len;
    unsigned int bv_offset;
};

/* Bio iterator structure */
struct bvec_iter {
    sector_t bi_sector;
    unsigned int bi_size;
    unsigned int bi_idx;
    unsigned int bi_bvec_done;
};

/* Bio structure */
struct bio {
    struct bio *bi_next;
    struct block_device *bi_bdev;
    unsigned long bi_opf;
    unsigned short bi_vcnt;
    unsigned short bi_flags;
    struct bvec_iter bi_iter;
    unsigned int bi_phys_segments;
    struct bio_vec *bi_io_vec;
    struct bio_set *bi_pool;
};

/* Request structure */
struct request {
    struct request *next;
    sector_t __sector;
    unsigned int __data_len;
    unsigned int nr_phys_segments;
    struct bio *bio;
    struct bio *biotail;
    struct request_queue *q;
    unsigned long flags;
};

/* Request queue structure */
struct request_queue {
    struct queue_limits limits;
    unsigned int sg_reserved_size;
    bool no_merge;
};

/* Block device structure */
struct block_device {
    sector_t bd_start_sect;
    sector_t bd_size;
    struct request_queue *bd_queue;
};

/* Helper functions */
static inline unsigned int queue_max_segments(const struct request_queue *q)
{
    return q->limits.max_segments;
}

static inline bool queue_virt_boundary(const struct request_queue *q)
{
    return q->limits.chunk_sectors;
}

static inline unsigned int bio_sectors(const struct bio *bio)
{
    return bio->bi_iter.bi_size >> SECTOR_SHIFT;
}

static inline bool bio_has_data(struct bio *bio)
{
    return bio && bio->bi_io_vec && bio->bi_vcnt;
}

/* Memory allocation functions */
static void *kmalloc(size_t size, int flags)
{
    return calloc(1, size);
}

static void kfree(void *ptr)
{
    free(ptr);
}

/* Bio vector operations */
static bool biovec_phys_mergeable(struct request_queue *q,
                                struct bio_vec *vec1,
                                struct bio_vec *vec2)
{
    phys_addr_t addr1 = (phys_addr_t)vec1->bv_page + vec1->bv_offset;
    phys_addr_t addr2 = (phys_addr_t)vec2->bv_page + vec2->bv_offset;

    if (addr1 + vec1->bv_len == addr2)
        return true;
    return false;
}

static bool __bvec_gap_to_prev(const struct queue_limits *lim,
                              struct bio_vec *bprv,
                              unsigned int offset)
{
    unsigned long prev_end = ((unsigned long)bprv->bv_page + bprv->bv_offset +
                             bprv->bv_len);
    unsigned long next_start = offset;

    return (next_start - prev_end) > lim->chunk_sectors << SECTOR_SHIFT;
}

/* Bio operations */
static struct bio *bio_alloc(unsigned int nr_vecs)
{
    struct bio *bio = kmalloc(sizeof(struct bio), 0);
    if (!bio)
        return NULL;

    bio->bi_io_vec = kmalloc(sizeof(struct bio_vec) * nr_vecs, 0);
    if (!bio->bi_io_vec) {
        kfree(bio);
        return NULL;
    }

    bio->bi_vcnt = nr_vecs;
    bio->bi_flags = 0;
    bio->bi_phys_segments = 0;
    return bio;
}

static void bio_free(struct bio *bio)
{
    if (bio) {
        kfree(bio->bi_io_vec);
        kfree(bio);
    }
}

/* Request operations */
static struct request *request_alloc(struct request_queue *q)
{
    struct request *rq = kmalloc(sizeof(struct request), 0);
    if (!rq)
        return NULL;

    rq->q = q;
    rq->bio = NULL;
    rq->biotail = NULL;
    rq->nr_phys_segments = 0;
    return rq;
}

static void request_free(struct request *rq)
{
    struct bio *bio = rq->bio;
    while (bio) {
        struct bio *next = bio->bi_next;
        bio_free(bio);
        bio = next;
    }
    kfree(rq);
}

/* Merge checking functions */
static bool blk_rq_merge_ok(struct request *rq, struct bio *bio)
{
    if ((rq->flags & REQ_ATOMIC) != (bio->bi_opf & REQ_ATOMIC))
        return false;

    if ((bio->bi_opf & REQ_OP_MASK) != (rq->flags & REQ_OP_MASK))
        return false;

    return true;
}

static bool req_gap_back_merge(struct request *req, struct bio *bio)
{
    struct bio_vec bv1, bv2;
    struct bio *last = req->biotail;

    if (!bio_has_data(last) || !queue_virt_boundary(req->q))
        return false;

    bv1 = last->bi_io_vec[last->bi_vcnt - 1];
    bv2 = bio->bi_io_vec[0];

    return __bvec_gap_to_prev(&req->q->limits, &bv1, bv2.bv_offset);
}

static bool req_gap_front_merge(struct request *req, struct bio *bio)
{
    struct bio_vec bv1, bv2;

    if (!bio_has_data(bio) || !queue_virt_boundary(req->q))
        return false;

    bv1 = bio->bi_io_vec[bio->bi_vcnt - 1];
    bv2 = req->bio->bi_io_vec[0];

    return __bvec_gap_to_prev(&req->q->limits, &bv1, bv2.bv_offset);
}

/* Main merge functions */
static bool attempt_back_merge(struct request_queue *q,
                             struct request *rq,
                             struct bio *bio)
{
    if (!blk_rq_merge_ok(rq, bio))
        return false;

    if (req_gap_back_merge(rq, bio))
        return false;

    if (bio_sectors(bio) + bio_sectors(rq->biotail) >
        q->limits.max_sectors)
        return false;

    return true;
}

static bool attempt_front_merge(struct request_queue *q,
                              struct request *rq,
                              struct bio *bio)
{
    if (!blk_rq_merge_ok(rq, bio))
        return false;

    if (req_gap_front_merge(rq, bio))
        return false;

    if (bio_sectors(bio) + bio_sectors(rq->bio) >
        q->limits.max_sectors)
        return false;

    return true;
}

/* Test utilities */
static void init_queue_limits(struct queue_limits *lim)
{
    lim->max_segments = QUEUE_MAX_SEGMENTS;
    lim->max_sectors = 256;
    lim->max_segment_size = PAGE_SIZE;
    lim->physical_block_size = 512;
    lim->logical_block_size = 512;
    lim->io_min = 512;
    lim->io_opt = 0;
    lim->max_discard_sectors = 256;
    lim->max_write_zeroes_sectors = 256;
    lim->discard_granularity = 512;
    lim->discard_alignment = 0;
    lim->chunk_sectors = 8;
    lim->atomic_write_max_sectors = 128;
    lim->atomic_write_boundary_sectors = 8;
}

static struct bio *create_test_bio(sector_t sector, unsigned int size,
                                 unsigned int op)
{
    struct bio *bio = bio_alloc(1);
    if (!bio)
        return NULL;

    bio->bi_iter.bi_sector = sector;
    bio->bi_iter.bi_size = size;
    bio->bi_opf = op;

    /* Allocate a dummy page for testing */
    bio->bi_io_vec[0].bv_page = malloc(PAGE_SIZE);
    bio->bi_io_vec[0].bv_len = size;
    bio->bi_io_vec[0].bv_offset = 0;

    return bio;
}

static void print_bio_info(struct bio *bio, const char *prefix)
{
    printf("%s: sector=%llu size=%u op=%u\n",
           prefix,
           (unsigned long long)bio->bi_iter.bi_sector,
           bio->bi_iter.bi_size,
           bio->bi_opf & REQ_OP_MASK);
}

static void print_request_info(struct request *rq, const char *prefix)
{
    struct bio *bio = rq->bio;
    printf("%s: sectors=%u phys_segments=%u\n",
           prefix, bio_sectors(bio), rq->nr_phys_segments);
}

/* Main test function */
int main(void)
{
    struct request_queue q = {0};
    struct request *rq;
    struct bio *bio1, *bio2, *bio3;
    bool merge_result;

    /* Initialize queue limits */
    init_queue_limits(&q.limits);
    q.no_merge = false;

    printf("Block I/O Merge Test Program\n");
    printf("============================\n\n");

    /* Test 1: Basic back merge */
    printf("Test 1: Basic back merge\n");
    rq = request_alloc(&q);
    if (!rq) {
        printf("Failed to allocate request\n");
        return -1;
    }

    /* Create first bio (request) */
    bio1 = create_test_bio(0, 4096, REQ_OP_WRITE);
    if (!bio1) {
        request_free(rq);
        printf("Failed to create bio1\n");
        return -1;
    }
    rq->bio = bio1;
    rq->biotail = bio1;
    print_bio_info(bio1, "Initial request bio");

    /* Create second bio for merging */
    bio2 = create_test_bio(8, 4096, REQ_OP_WRITE);
    if (!bio2) {
        request_free(rq);
        printf("Failed to create bio2\n");
        return -1;
    }
    print_bio_info(bio2, "Bio to merge");

    /* Attempt back merge */
    merge_result = attempt_back_merge(&q, rq, bio2);
    printf("Back merge result: %s\n\n", merge_result ? "SUCCESS" : "FAILED");

    /* Test 2: Front merge */
    printf("Test 2: Front merge\n");
    bio3 = create_test_bio(16, 4096, REQ_OP_WRITE);
    if (!bio3) {
        bio_free(bio2);
        request_free(rq);
        printf("Failed to create bio3\n");
        return -1;
    }
    print_bio_info(bio3, "Bio to front merge");

    /* Attempt front merge */
    merge_result = attempt_front_merge(&q, rq, bio3);
    printf("Front merge result: %s\n\n", merge_result ? "SUCCESS" : "FAILED");

    /* Test 3: Merge with different operations */
    printf("Test 3: Merge with different operations\n");
    bio3->bi_opf = REQ_OP_READ;
    print_bio_info(bio3, "Bio with different operation");
    merge_result = attempt_front_merge(&q, rq, bio3);
    printf("Front merge result (different ops): %s\n\n",
           merge_result ? "SUCCESS" : "FAILED");

    /* Test 4: Merge exceeding max sectors */
    printf("Test 4: Merge exceeding max sectors\n");
    bio3->bi_iter.bi_size = 256 * 512; /* Max sectors */
    bio3->bi_opf = REQ_OP_WRITE;
    print_bio_info(bio3, "Bio with large size");
    merge_result = attempt_front_merge(&q, rq, bio3);
    printf("Front merge result (large size): %s\n\n",
           merge_result ? "SUCCESS" : "FAILED");

    /* Test 5: Merge with atomic write flag */
    printf("Test 5: Merge with atomic write flag\n");
    rq->flags |= REQ_ATOMIC;
    bio3->bi_opf = REQ_OP_WRITE;
    print_bio_info(bio3, "Bio without atomic flag");
    merge_result = attempt_front_merge(&q, rq, bio3);
    printf("Front merge result (atomic mismatch): %s\n\n",
           merge_result ? "SUCCESS" : "FAILED");

    /* Cleanup */
    bio_free(bio2);
    bio_free(bio3);
    request_free(rq);

    printf("All tests completed.\n");
    return 0;
}
