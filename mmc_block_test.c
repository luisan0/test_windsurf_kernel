/*
 * MMC Block Device Test Program
 * This is a standalone simulation of MMC block device operations
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
#include <time.h>

/* Simple queue implementation */
#define QUEUE_ENTRY(type) \
    struct { \
        struct type *next; \
        struct type *prev; \
    }

#define QUEUE_HEAD(name, type) \
    struct name { \
        struct type *first; \
        struct type *last; \
    }

#define QUEUE_INIT(head) do { \
    (head)->first = NULL; \
    (head)->last = NULL; \
} while (0)

#define QUEUE_INSERT_TAIL(head, elm, field) do { \
    if ((head)->last) { \
        (head)->last->field.next = (elm); \
        (elm)->field.prev = (head)->last; \
        (elm)->field.next = NULL; \
        (head)->last = (elm); \
    } else { \
        (head)->first = (elm); \
        (head)->last = (elm); \
        (elm)->field.next = NULL; \
        (elm)->field.prev = NULL; \
    } \
} while (0)

#define QUEUE_REMOVE(head, elm, field) do { \
    if ((elm)->field.prev) \
        (elm)->field.prev->field.next = (elm)->field.next; \
    else \
        (head)->first = (elm)->field.next; \
    if ((elm)->field.next) \
        (elm)->field.next->field.prev = (elm)->field.prev; \
    else \
        (head)->last = (elm)->field.prev; \
} while (0)

#define QUEUE_FIRST(head) ((head)->first)
#define QUEUE_EMPTY(head) ((head)->first == NULL)

/* MMC/SD command definitions */
#define MMC_READ_SINGLE_BLOCK     17
#define MMC_WRITE_BLOCK          24
#define MMC_SET_BLOCKLEN         16
#define MMC_SEND_STATUS          13
#define MMC_STOP_TRANSMISSION    12
#define MMC_SEND_OP_COND          1
#define MMC_ALL_SEND_CID          2
#define MMC_SET_RELATIVE_ADDR     3

/* Block device constants */
#define MMC_MAX_DEVICES          10
#define MMC_MIN_BLOCK_SIZE      512
#define MMC_MAX_BLOCK_SIZE     4096
#define MMC_DEFAULT_BLOCK_SIZE  512
#define MMC_MAX_BLOCKS       524288  /* 256MB at 512B per block */
#define MMC_MAX_REQ_SIZE      4096
#define MMC_MAX_RETRIES         10

/* Request flags */
#define MMC_REQ_SYNC        (1U << 0)
#define MMC_REQ_SPECIAL     (1U << 1)
#define MMC_REQ_URGENT      (1U << 2)
#define MMC_REQ_WRITE       (1U << 3)
#define MMC_REQ_DONE        (1U << 4)
#define MMC_REQ_FAILED      (1U << 5)

/* Card states */
enum mmc_state {
    MMC_STATE_PRESENT,
    MMC_STATE_READONLY,
    MMC_STATE_HIGHSPEED,
    MMC_STATE_BLOCKADDR,
    MMC_STATE_SUSPENDED,
    MMC_STATE_REMOVED,
    MMC_STATE_ERROR
};

/* Request structure */
struct mmc_request {
    uint32_t cmd;              /* Command to execute */
    uint32_t arg;              /* Command argument */
    uint32_t flags;            /* Request flags */
    uint32_t retries;          /* Retry count */
    void *data;                /* Data buffer */
    size_t len;                /* Data length */
    int error;                 /* Error code */
    void (*complete)(struct mmc_request *); /* Completion callback */
    QUEUE_ENTRY(mmc_request) queue; /* Queue entry */
};

/* Queue head structure */
QUEUE_HEAD(mmc_queue_head, mmc_request);

/* Block device structure */
struct mmc_blk_dev {
    int id;                     /* Device ID */
    char name[32];             /* Device name */
    enum mmc_state state;      /* Card state */
    uint32_t flags;            /* Device flags */
    uint32_t read_only;        /* Read-only flag */
    uint32_t block_size;       /* Block size */
    uint32_t blocks;           /* Number of blocks */
    uint64_t capacity;         /* Total capacity */
    
    /* Request queue */
    struct mmc_queue_head queue;
    pthread_mutex_t queue_lock;
    pthread_cond_t queue_cond;
    
    /* Worker thread */
    pthread_t worker_thread;
    bool worker_running;
    
    /* Statistics */
    uint64_t reads;
    uint64_t writes;
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint64_t errors;
    
    /* Simulated storage */
    uint8_t *storage;
};

/* Global variables */
static struct mmc_blk_dev *mmc_devices[MMC_MAX_DEVICES];
static pthread_mutex_t mmc_lock = PTHREAD_MUTEX_INITIALIZER;
static int mmc_dev_count = 0;

/* Function declarations */
static struct mmc_blk_dev *mmc_alloc_dev(void);
static void mmc_free_dev(struct mmc_blk_dev *dev);
static int mmc_add_dev(struct mmc_blk_dev *dev);
static void mmc_remove_dev(struct mmc_blk_dev *dev);
static struct mmc_request *mmc_alloc_request(uint32_t cmd, uint32_t arg);
static void mmc_free_request(struct mmc_request *req);
static int mmc_queue_request(struct mmc_blk_dev *dev, struct mmc_request *req);
static void mmc_complete_request(struct mmc_request *req);
static void *mmc_worker_thread(void *arg);
static int mmc_process_request(struct mmc_blk_dev *dev, struct mmc_request *req);
static void mmc_dump_stats(struct mmc_blk_dev *dev);

/* Allocate block device */
static struct mmc_blk_dev *mmc_alloc_dev(void) {
    struct mmc_blk_dev *dev;
    
    dev = calloc(1, sizeof(*dev));
    if (!dev)
        return NULL;
    
    /* Initialize queue */
    QUEUE_INIT(&dev->queue);
    pthread_mutex_init(&dev->queue_lock, NULL);
    pthread_cond_init(&dev->queue_cond, NULL);
    
    /* Set default values */
    dev->block_size = MMC_DEFAULT_BLOCK_SIZE;
    dev->blocks = MMC_MAX_BLOCKS;
    dev->capacity = (uint64_t)dev->block_size * dev->blocks;
    
    /* Allocate storage */
    dev->storage = calloc(1, dev->capacity);
    if (!dev->storage) {
        free(dev);
        return NULL;
    }
    
    return dev;
}

/* Free block device */
static void mmc_free_dev(struct mmc_blk_dev *dev) {
    if (!dev)
        return;
    
    /* Stop worker thread */
    dev->worker_running = false;
    pthread_cond_signal(&dev->queue_cond);
    pthread_join(dev->worker_thread, NULL);
    
    /* Free queued requests */
    struct mmc_request *req;
    while ((req = QUEUE_FIRST(&dev->queue))) {
        QUEUE_REMOVE(&dev->queue, req, queue);
        mmc_free_request(req);
    }
    
    /* Cleanup */
    pthread_mutex_destroy(&dev->queue_lock);
    pthread_cond_destroy(&dev->queue_cond);
    free(dev->storage);
    free(dev);
}

/* Add block device */
static int mmc_add_dev(struct mmc_blk_dev *dev) {
    pthread_mutex_lock(&mmc_lock);
    
    if (mmc_dev_count >= MMC_MAX_DEVICES) {
        pthread_mutex_unlock(&mmc_lock);
        return -ENOSPC;
    }
    
    /* Find free slot */
    int id;
    for (id = 0; id < MMC_MAX_DEVICES; id++) {
        if (!mmc_devices[id])
            break;
    }
    
    /* Initialize device */
    dev->id = id;
    snprintf(dev->name, sizeof(dev->name), "mmcblk%d", id);
    dev->state = MMC_STATE_PRESENT;
    dev->worker_running = true;
    
    /* Start worker thread */
    pthread_create(&dev->worker_thread, NULL, mmc_worker_thread, dev);
    
    /* Add to array */
    mmc_devices[id] = dev;
    mmc_dev_count++;
    
    pthread_mutex_unlock(&mmc_lock);
    return 0;
}

/* Remove block device */
static void mmc_remove_dev(struct mmc_blk_dev *dev) {
    if (!dev)
        return;
    
    pthread_mutex_lock(&mmc_lock);
    
    if (mmc_devices[dev->id] == dev) {
        mmc_devices[dev->id] = NULL;
        mmc_dev_count--;
    }
    
    pthread_mutex_unlock(&mmc_lock);
    
    mmc_free_dev(dev);
}

/* Allocate request */
static struct mmc_request *mmc_alloc_request(uint32_t cmd, uint32_t arg) {
    struct mmc_request *req = calloc(1, sizeof(*req));
    if (!req)
        return NULL;
    
    req->cmd = cmd;
    req->arg = arg;
    
    return req;
}

/* Free request */
static void mmc_free_request(struct mmc_request *req) {
    if (!req)
        return;
    
    free(req->data);
    free(req);
}

/* Queue request */
static int mmc_queue_request(struct mmc_blk_dev *dev, struct mmc_request *req) {
    pthread_mutex_lock(&dev->queue_lock);
    
    /* Add to queue */
    QUEUE_INSERT_TAIL(&dev->queue, req, queue);
    
    /* Signal worker */
    pthread_cond_signal(&dev->queue_cond);
    
    pthread_mutex_unlock(&dev->queue_lock);
    return 0;
}

/* Complete request */
static void mmc_complete_request(struct mmc_request *req) {
    if (req->error)
        req->flags |= MMC_REQ_FAILED;
    
    req->flags |= MMC_REQ_DONE;
    
    if (req->complete)
        req->complete(req);
    else
        mmc_free_request(req);
}

/* Process request */
static int mmc_process_request(struct mmc_blk_dev *dev, struct mmc_request *req) {
    uint32_t block, count;
    uint8_t *buf;
    int ret = 0;
    
    switch (req->cmd) {
    case MMC_READ_SINGLE_BLOCK:
        block = req->arg;
        if (block >= dev->blocks) {
            ret = -EINVAL;
            break;
        }
        
        buf = req->data;
        if (!buf) {
            ret = -EINVAL;
            break;
        }
        
        /* Simulate read delay */
        usleep(1000);
        
        /* Copy data */
        memcpy(buf, dev->storage + (block * dev->block_size), dev->block_size);
        
        dev->reads++;
        dev->read_bytes += dev->block_size;
        break;
        
    case MMC_WRITE_BLOCK:
        if (dev->read_only) {
            ret = -EROFS;
            break;
        }
        
        block = req->arg;
        if (block >= dev->blocks) {
            ret = -EINVAL;
            break;
        }
        
        buf = req->data;
        if (!buf) {
            ret = -EINVAL;
            break;
        }
        
        /* Simulate write delay */
        usleep(2000);
        
        /* Copy data */
        memcpy(dev->storage + (block * dev->block_size), buf, dev->block_size);
        
        dev->writes++;
        dev->write_bytes += dev->block_size;
        break;
        
    case MMC_SET_BLOCKLEN:
        count = req->arg;
        if (count < MMC_MIN_BLOCK_SIZE || count > MMC_MAX_BLOCK_SIZE) {
            ret = -EINVAL;
            break;
        }
        dev->block_size = count;
        break;
        
    case MMC_SEND_STATUS:
        /* Just succeed */
        break;
        
    default:
        printf("Unknown command: %u\n", req->cmd);
        ret = -EINVAL;
        break;
    }
    
    if (ret)
        dev->errors++;
    
    return ret;
}

/* Worker thread */
static void *mmc_worker_thread(void *arg) {
    struct mmc_blk_dev *dev = arg;
    struct mmc_request *req;
    
    while (dev->worker_running) {
        pthread_mutex_lock(&dev->queue_lock);
        
        while (dev->worker_running && QUEUE_EMPTY(&dev->queue))
            pthread_cond_wait(&dev->queue_cond, &dev->queue_lock);
        
        if (!dev->worker_running) {
            pthread_mutex_unlock(&dev->queue_lock);
            break;
        }
        
        /* Get next request */
        req = QUEUE_FIRST(&dev->queue);
        QUEUE_REMOVE(&dev->queue, req, queue);
        
        pthread_mutex_unlock(&dev->queue_lock);
        
        /* Process request */
        req->error = mmc_process_request(dev, req);
        
        /* Complete request */
        mmc_complete_request(req);
    }
    
    return NULL;
}

/* Dump statistics */
static void mmc_dump_stats(struct mmc_blk_dev *dev) {
    printf("\nMMC Block Device Statistics (%s):\n", dev->name);
    printf("================================\n");
    printf("State: %d\n", dev->state);
    printf("Block size: %u bytes\n", dev->block_size);
    printf("Blocks: %u\n", dev->blocks);
    printf("Capacity: %lu bytes\n", dev->capacity);
    printf("Read-only: %s\n", dev->read_only ? "yes" : "no");
    printf("Reads: %lu\n", dev->reads);
    printf("Writes: %lu\n", dev->writes);
    printf("Read bytes: %lu\n", dev->read_bytes);
    printf("Write bytes: %lu\n", dev->write_bytes);
    printf("Errors: %lu\n", dev->errors);
}

/* Test functions */
static void test_basic_io(struct mmc_blk_dev *dev) {
    printf("\nTesting basic I/O operations...\n");
    
    /* Prepare test data */
    uint8_t write_buf[512];
    uint8_t read_buf[512];
    for (int i = 0; i < sizeof(write_buf); i++)
        write_buf[i] = i & 0xff;
    
    /* Write test */
    struct mmc_request *req = mmc_alloc_request(MMC_WRITE_BLOCK, 0);
    if (!req) {
        printf("Failed to allocate write request\n");
        return;
    }
    
    req->data = malloc(sizeof(write_buf));
    if (!req->data) {
        mmc_free_request(req);
        printf("Failed to allocate write buffer\n");
        return;
    }
    
    memcpy(req->data, write_buf, sizeof(write_buf));
    req->len = sizeof(write_buf);
    
    printf("Writing %zu bytes to block 0...\n", req->len);
    mmc_queue_request(dev, req);
    
    /* Wait for write to complete */
    usleep(5000);
    
    /* Read test */
    req = mmc_alloc_request(MMC_READ_SINGLE_BLOCK, 0);
    if (!req) {
        printf("Failed to allocate read request\n");
        return;
    }
    
    req->data = malloc(sizeof(read_buf));
    if (!req->data) {
        mmc_free_request(req);
        printf("Failed to allocate read buffer\n");
        return;
    }
    
    printf("Reading %zu bytes from block 0...\n", sizeof(read_buf));
    mmc_queue_request(dev, req);
    
    /* Wait for read to complete */
    usleep(5000);
    
    /* Verify data */
    if (memcmp(write_buf, req->data, sizeof(write_buf)) == 0)
        printf("Data verification successful!\n");
    else
        printf("Data verification failed!\n");
}

static void test_error_handling(struct mmc_blk_dev *dev) {
    printf("\nTesting error handling...\n");
    
    /* Test invalid block */
    struct mmc_request *req = mmc_alloc_request(MMC_READ_SINGLE_BLOCK, dev->blocks + 1);
    if (!req) {
        printf("Failed to allocate request\n");
        return;
    }
    
    req->data = malloc(dev->block_size);
    if (!req->data) {
        mmc_free_request(req);
        printf("Failed to allocate buffer\n");
        return;
    }
    
    printf("Attempting to read invalid block %u...\n", dev->blocks + 1);
    mmc_queue_request(dev, req);
    
    /* Wait for completion */
    usleep(5000);
    
    /* Test read-only violation */
    dev->read_only = 1;
    
    req = mmc_alloc_request(MMC_WRITE_BLOCK, 0);
    if (!req) {
        printf("Failed to allocate request\n");
        return;
    }
    
    req->data = malloc(dev->block_size);
    if (!req->data) {
        mmc_free_request(req);
        printf("Failed to allocate buffer\n");
        return;
    }
    
    printf("Attempting to write to read-only device...\n");
    mmc_queue_request(dev, req);
    
    /* Wait for completion */
    usleep(5000);
    
    dev->read_only = 0;
}

int main(void) {
    printf("MMC Block Device Test Program\n");
    printf("============================\n\n");
    
    /* Create device */
    struct mmc_blk_dev *dev = mmc_alloc_dev();
    if (!dev) {
        printf("Failed to allocate device\n");
        return 1;
    }
    
    /* Add device */
    int ret = mmc_add_dev(dev);
    if (ret < 0) {
        printf("Failed to add device: %d\n", ret);
        mmc_free_dev(dev);
        return 1;
    }
    
    printf("Created device %s\n", dev->name);
    printf("Block size: %u bytes\n", dev->block_size);
    printf("Number of blocks: %u\n", dev->blocks);
    printf("Total capacity: %lu bytes\n", dev->capacity);
    
    /* Run tests */
    test_basic_io(dev);
    test_error_handling(dev);
    
    /* Display statistics */
    mmc_dump_stats(dev);
    
    /* Cleanup */
    mmc_remove_dev(dev);
    
    printf("\nTest completed successfully!\n");
    return 0;
}
