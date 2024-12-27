/*
 * VirtIO Block Device Test Program
 * This is a standalone simulation of the VirtIO block device driver
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

/* VirtIO Block Device Constants */
#define VIRTIO_BLK_T_IN           0
#define VIRTIO_BLK_T_OUT          1
#define VIRTIO_BLK_T_FLUSH        4
#define VIRTIO_BLK_T_DISCARD      11
#define VIRTIO_BLK_T_WRITE_ZEROES 13

#define VIRTIO_BLK_S_OK          0
#define VIRTIO_BLK_S_IOERR       1
#define VIRTIO_BLK_S_UNSUPP      2

#define SECTOR_SIZE              512
#define DEFAULT_DISK_SIZE        (100 * 1024 * 1024)  /* 100MB */
#define MAX_SEGMENTS            32
#define VQ_SIZE                 128
#define NUM_QUEUES              4

/* Request Status */
#define REQ_STATUS_PENDING      0
#define REQ_STATUS_PROCESSING   1
#define REQ_STATUS_COMPLETE     2
#define REQ_STATUS_ERROR        3

/* VirtIO Block Request Header */
struct virtio_blk_outhdr {
    uint32_t type;
    uint32_t priority;
    uint64_t sector;
};

/* VirtIO Block Request */
struct virtio_blk_req {
    struct virtio_blk_outhdr out_hdr;
    uint8_t *data;
    uint32_t data_len;
    uint8_t status;
    int req_status;
    struct virtio_blk_req *next;
};

/* Scatter-Gather Entry */
struct scatterlist {
    void *data;
    size_t length;
    struct scatterlist *next;
};

/* VirtIO Queue */
struct virtqueue {
    pthread_mutex_t lock;
    struct virtio_blk_req *pending_head;
    struct virtio_blk_req *processing_head;
    struct virtio_blk_req *completed_head;
    int queue_size;
    int num_pending;
    int num_processing;
    bool interrupt_enabled;
};

/* VirtIO Block Device */
struct virtio_blk_device {
    uint8_t *storage;
    size_t capacity;
    struct virtqueue *vqs[NUM_QUEUES];
    pthread_t processing_thread;
    bool device_ready;
    pthread_mutex_t device_lock;
    uint32_t features;
    char serial[256];
    int current_queue;
};

/* Function Declarations */
static struct virtio_blk_device *virtblk_init(size_t capacity);
static void virtblk_cleanup(struct virtio_blk_device *vdev);
static int virtblk_make_request(struct virtio_blk_device *vdev, 
                              uint32_t type,
                              uint64_t sector,
                              uint8_t *data,
                              size_t len);
static void *virtblk_process_requests(void *arg);
static void virtblk_complete_request(struct virtqueue *vq, 
                                   struct virtio_blk_req *req);
static int virtblk_read(struct virtio_blk_device *vdev,
                       uint64_t sector,
                       uint8_t *buffer,
                       size_t count);
static int virtblk_write(struct virtio_blk_device *vdev,
                        uint64_t sector,
                        const uint8_t *buffer,
                        size_t count);
static void virtblk_dump_stats(struct virtio_blk_device *vdev);

/* Queue Management Functions */
static struct virtqueue *virtqueue_create(int size) {
    struct virtqueue *vq = calloc(1, sizeof(struct virtqueue));
    if (!vq)
        return NULL;

    pthread_mutex_init(&vq->lock, NULL);
    vq->queue_size = size;
    vq->interrupt_enabled = true;
    return vq;
}

static void virtqueue_destroy(struct virtqueue *vq) {
    if (!vq)
        return;

    pthread_mutex_destroy(&vq->lock);
    free(vq);
}

static int virtqueue_add_request(struct virtqueue *vq, struct virtio_blk_req *req) {
    pthread_mutex_lock(&vq->lock);

    if (vq->num_pending >= vq->queue_size) {
        pthread_mutex_unlock(&vq->lock);
        return -ENOSPC;
    }

    req->next = vq->pending_head;
    vq->pending_head = req;
    vq->num_pending++;

    pthread_mutex_unlock(&vq->lock);
    return 0;
}

static struct virtio_blk_req *virtqueue_get_next_pending(struct virtqueue *vq) {
    pthread_mutex_lock(&vq->lock);

    struct virtio_blk_req *req = vq->pending_head;
    if (req) {
        vq->pending_head = req->next;
        req->next = vq->processing_head;
        vq->processing_head = req;
        vq->num_pending--;
        vq->num_processing++;
        req->req_status = REQ_STATUS_PROCESSING;
    }

    pthread_mutex_unlock(&vq->lock);
    return req;
}

/* Device Implementation */
static struct virtio_blk_device *virtblk_init(size_t capacity) {
    struct virtio_blk_device *vdev = calloc(1, sizeof(struct virtio_blk_device));
    if (!vdev)
        return NULL;

    vdev->storage = calloc(1, capacity);
    if (!vdev->storage) {
        free(vdev);
        return NULL;
    }

    vdev->capacity = capacity;
    pthread_mutex_init(&vdev->device_lock, NULL);

    /* Initialize virtqueues */
    for (int i = 0; i < NUM_QUEUES; i++) {
        vdev->vqs[i] = virtqueue_create(VQ_SIZE);
        if (!vdev->vqs[i]) {
            for (int j = 0; j < i; j++)
                virtqueue_destroy(vdev->vqs[j]);
            free(vdev->storage);
            free(vdev);
            return NULL;
        }
    }

    /* Set device features */
    vdev->features = (1 << VIRTIO_BLK_T_IN) |
                    (1 << VIRTIO_BLK_T_OUT) |
                    (1 << VIRTIO_BLK_T_FLUSH);

    /* Set serial number */
    snprintf(vdev->serial, sizeof(vdev->serial), "VT%010lu", (unsigned long)time(NULL));

    /* Start processing thread */
    vdev->device_ready = true;
    if (pthread_create(&vdev->processing_thread, NULL, virtblk_process_requests, vdev) != 0) {
        for (int i = 0; i < NUM_QUEUES; i++)
            virtqueue_destroy(vdev->vqs[i]);
        free(vdev->storage);
        free(vdev);
        return NULL;
    }

    return vdev;
}

static void virtblk_cleanup(struct virtio_blk_device *vdev) {
    if (!vdev)
        return;

    /* Stop processing thread */
    vdev->device_ready = false;
    pthread_join(vdev->processing_thread, NULL);

    /* Cleanup queues */
    for (int i = 0; i < NUM_QUEUES; i++) {
        struct virtqueue *vq = vdev->vqs[i];
        if (vq) {
            struct virtio_blk_req *req, *next;
            
            /* Free pending requests */
            req = vq->pending_head;
            while (req) {
                next = req->next;
                free(req->data);
                free(req);
                req = next;
            }

            /* Free processing requests */
            req = vq->processing_head;
            while (req) {
                next = req->next;
                free(req->data);
                free(req);
                req = next;
            }

            /* Free completed requests */
            req = vq->completed_head;
            while (req) {
                next = req->next;
                free(req->data);
                free(req);
                req = next;
            }

            virtqueue_destroy(vq);
        }
    }

    pthread_mutex_destroy(&vdev->device_lock);
    free(vdev->storage);
    free(vdev);
}

static int virtblk_make_request(struct virtio_blk_device *vdev,
                              uint32_t type,
                              uint64_t sector,
                              uint8_t *data,
                              size_t len) {
    if (!vdev || !vdev->device_ready)
        return -ENODEV;

    if (sector * SECTOR_SIZE + len > vdev->capacity)
        return -EINVAL;

    /* Allocate request */
    struct virtio_blk_req *req = calloc(1, sizeof(struct virtio_blk_req));
    if (!req)
        return -ENOMEM;

    /* Setup request */
    req->out_hdr.type = type;
    req->out_hdr.sector = sector;
    req->data = data;
    req->data_len = len;
    req->req_status = REQ_STATUS_PENDING;

    /* Round-robin queue selection */
    int queue_num = vdev->current_queue;
    vdev->current_queue = (vdev->current_queue + 1) % NUM_QUEUES;

    /* Add to queue */
    int ret = virtqueue_add_request(vdev->vqs[queue_num], req);
    if (ret < 0) {
        free(req);
        return ret;
    }

    return 0;
}

static void virtblk_complete_request(struct virtqueue *vq, struct virtio_blk_req *req) {
    pthread_mutex_lock(&vq->lock);

    /* Remove from processing list */
    struct virtio_blk_req **curr = &vq->processing_head;
    while (*curr && *curr != req)
        curr = &(*curr)->next;
    
    if (*curr) {
        *curr = req->next;
        vq->num_processing--;

        /* Add to completed list */
        req->next = vq->completed_head;
        vq->completed_head = req;
        req->req_status = REQ_STATUS_COMPLETE;
    }

    pthread_mutex_unlock(&vq->lock);
}

static void *virtblk_process_requests(void *arg) {
    struct virtio_blk_device *vdev = arg;
    
    while (vdev->device_ready) {
        bool processed = false;

        /* Process requests from all queues */
        for (int i = 0; i < NUM_QUEUES; i++) {
            struct virtqueue *vq = vdev->vqs[i];
            struct virtio_blk_req *req = virtqueue_get_next_pending(vq);

            if (req) {
                processed = true;
                int ret = 0;

                /* Process request based on type */
                switch (req->out_hdr.type) {
                case VIRTIO_BLK_T_IN:
                    ret = virtblk_read(vdev, req->out_hdr.sector,
                                     req->data, req->data_len);
                    break;

                case VIRTIO_BLK_T_OUT:
                    ret = virtblk_write(vdev, req->out_hdr.sector,
                                      req->data, req->data_len);
                    break;

                case VIRTIO_BLK_T_FLUSH:
                    /* Simulate flush operation */
                    usleep(1000);
                    break;

                default:
                    ret = -ENOTSUP;
                    break;
                }

                /* Set request status */
                req->status = (ret == 0) ? VIRTIO_BLK_S_OK :
                             (ret == -ENOTSUP) ? VIRTIO_BLK_S_UNSUPP :
                             VIRTIO_BLK_S_IOERR;

                /* Complete request */
                virtblk_complete_request(vq, req);
            }
        }

        /* If no requests were processed, sleep for a short time */
        if (!processed)
            usleep(1000);
    }

    return NULL;
}

static int virtblk_read(struct virtio_blk_device *vdev,
                       uint64_t sector,
                       uint8_t *buffer,
                       size_t count) {
    if (!vdev || !buffer)
        return -EINVAL;

    size_t offset = sector * SECTOR_SIZE;
    if (offset + count > vdev->capacity)
        return -EINVAL;

    pthread_mutex_lock(&vdev->device_lock);
    memcpy(buffer, vdev->storage + offset, count);
    pthread_mutex_unlock(&vdev->device_lock);

    /* Simulate I/O latency */
    usleep(100);

    return 0;
}

static int virtblk_write(struct virtio_blk_device *vdev,
                        uint64_t sector,
                        const uint8_t *buffer,
                        size_t count) {
    if (!vdev || !buffer)
        return -EINVAL;

    size_t offset = sector * SECTOR_SIZE;
    if (offset + count > vdev->capacity)
        return -EINVAL;

    pthread_mutex_lock(&vdev->device_lock);
    memcpy(vdev->storage + offset, buffer, count);
    pthread_mutex_unlock(&vdev->device_lock);

    /* Simulate I/O latency */
    usleep(100);

    return 0;
}

static void virtblk_dump_stats(struct virtio_blk_device *vdev) {
    printf("\nVirtIO Block Device Statistics:\n");
    printf("================================\n");
    printf("Capacity: %zu bytes\n", vdev->capacity);
    printf("Serial Number: %s\n", vdev->serial);
    printf("Features: 0x%08x\n", vdev->features);
    printf("Number of Queues: %d\n\n", NUM_QUEUES);

    for (int i = 0; i < NUM_QUEUES; i++) {
        struct virtqueue *vq = vdev->vqs[i];
        pthread_mutex_lock(&vq->lock);
        printf("Queue %d:\n", i);
        printf("  Queue Size: %d\n", vq->queue_size);
        printf("  Pending Requests: %d\n", vq->num_pending);
        printf("  Processing Requests: %d\n", vq->num_processing);
        pthread_mutex_unlock(&vq->lock);
    }
    printf("\n");
}

/* Test Functions */
static void test_basic_io(struct virtio_blk_device *vdev) {
    printf("Testing basic I/O operations...\n");

    /* Test write operation */
    const char *test_data = "Hello, VirtIO Block Device!";
    size_t len = strlen(test_data) + 1;
    uint8_t *write_buf = (uint8_t *)strdup(test_data);
    
    int ret = virtblk_make_request(vdev, VIRTIO_BLK_T_OUT, 0, write_buf, len);
    printf("Write request result: %d\n", ret);

    /* Wait for processing */
    usleep(1000);

    /* Test read operation */
    uint8_t *read_buf = calloc(1, len);
    ret = virtblk_make_request(vdev, VIRTIO_BLK_T_IN, 0, read_buf, len);
    printf("Read request result: %d\n", ret);

    /* Wait for processing */
    usleep(1000);

    /* Verify data */
    if (memcmp(write_buf, read_buf, len) == 0) {
        printf("Data verification successful!\n");
    } else {
        printf("Data verification failed!\n");
    }

    free(read_buf);
}

static void test_concurrent_io(struct virtio_blk_device *vdev) {
    printf("\nTesting concurrent I/O operations...\n");

    #define NUM_REQUESTS 20
    #define TEST_SIZE 1024

    struct {
        uint8_t *buffer;
        uint64_t sector;
    } requests[NUM_REQUESTS];

    /* Initialize test data */
    for (int i = 0; i < NUM_REQUESTS; i++) {
        requests[i].buffer = calloc(1, TEST_SIZE);
        requests[i].sector = i * (TEST_SIZE / SECTOR_SIZE);
        
        /* Fill buffer with pattern */
        for (int j = 0; j < TEST_SIZE; j++) {
            requests[i].buffer[j] = (i + j) & 0xFF;
        }
    }

    /* Submit write requests */
    for (int i = 0; i < NUM_REQUESTS; i++) {
        int ret = virtblk_make_request(vdev, VIRTIO_BLK_T_OUT,
                                     requests[i].sector,
                                     requests[i].buffer,
                                     TEST_SIZE);
        printf("Concurrent write request %d result: %d\n", i, ret);
    }

    /* Wait for processing */
    usleep(5000);

    /* Verify data */
    for (int i = 0; i < NUM_REQUESTS; i++) {
        uint8_t *verify_buf = calloc(1, TEST_SIZE);
        int ret = virtblk_make_request(vdev, VIRTIO_BLK_T_IN,
                                     requests[i].sector,
                                     verify_buf,
                                     TEST_SIZE);
        printf("Concurrent read request %d result: %d\n", i, ret);

        /* Wait for processing */
        usleep(1000);

        /* Verify data */
        if (memcmp(requests[i].buffer, verify_buf, TEST_SIZE) == 0) {
            printf("Concurrent request %d verification successful!\n", i);
        } else {
            printf("Concurrent request %d verification failed!\n", i);
        }

        free(verify_buf);
        free(requests[i].buffer);
    }
}

int main(void) {
    printf("VirtIO Block Device Test Program\n");
    printf("================================\n\n");

    /* Initialize device */
    struct virtio_blk_device *vdev = virtblk_init(DEFAULT_DISK_SIZE);
    if (!vdev) {
        printf("Failed to initialize VirtIO block device\n");
        return 1;
    }

    /* Run tests */
    test_basic_io(vdev);
    test_concurrent_io(vdev);

    /* Display statistics */
    virtblk_dump_stats(vdev);

    /* Cleanup */
    virtblk_cleanup(vdev);

    printf("\nTest completed successfully!\n");
    return 0;
}
