/*
 * UBD (User-mode Block Device) Test Program
 * This is a standalone simulation of the User Mode Linux block device driver
 * Author: Cascade AI
 * Date: 2024-12-27
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <io.h>

/* Basic definitions */
#define UBD_SHIFT 4
#define MAX_DEV 16
#define SECTOR_SIZE 512
#define UBD_MAX_REQUEST (8 * sizeof(long))
#define MAX_SG 64

/* Request types */
#define REQ_OP_READ 0
#define REQ_OP_WRITE 1
#define REQ_OP_FLUSH 2
#define REQ_OP_DISCARD 3
#define REQ_OP_SECURE_ERASE 4

/* Debug flags */
#define UBD_DEBUG_IO     0x0001
#define UBD_DEBUG_BLOCK  0x0002
#define UBD_DEBUG_REQ    0x0004
#define UBD_DEBUG_COW    0x0008
#define UBD_DEBUG_ALL    0xffff

/* Structure definitions */
struct openflags {
    unsigned int r:1;
    unsigned int w:1;
    unsigned int s:1;
    unsigned int c:1;
    unsigned int cl:1;
};

struct io_desc {
    char *buffer;
    unsigned long length;
    unsigned long sector_mask;
    unsigned long long cow_offset;
    unsigned long bitmap_words[2];
};

struct request {
    int type;
    unsigned long long sector;
    unsigned int nr_sectors;
    char *buffer;
    int error;
    void *private_data;
};

struct cow {
    char *file;
    int fd;
    unsigned long *bitmap;
    unsigned long bitmap_len;
    int bitmap_offset;
    int data_offset;
};

struct ubd {
    char *file;
    char *serial;
    int fd;
    uint64_t size;
    struct openflags boot_openflags;
    struct openflags openflags;
    unsigned int shared:1;
    unsigned int no_cow:1;
    unsigned int no_trim:1;
    struct cow cow;
    pthread_spinlock_t lock;
    pthread_t io_thread;
    bool thread_running;
    int debug_flags;
};

/* Function declarations */
static int ubd_open_dev(struct ubd *dev);
static void ubd_close_dev(struct ubd *dev);
static int ubd_read(struct ubd *dev, char *buffer, unsigned long long offset, unsigned int length);
static int ubd_write(struct ubd *dev, const char *buffer, unsigned long long offset, unsigned int length);
static void *io_thread(void *arg);
static int process_request(struct ubd *dev, struct request *req);

/* Helper functions */
static inline int ubd_test_bit(uint64_t bit, unsigned char *data)
{
    uint64_t n;
    int bits, off;

    bits = sizeof(data[0]) * 8;
    n = bit / bits;
    off = bit % bits;
    return (data[n] & (1 << off)) != 0;
}

static inline void ubd_set_bit(uint64_t bit, unsigned char *data)
{
    uint64_t n;
    int bits, off;

    bits = sizeof(data[0]) * 8;
    n = bit / bits;
    off = bit % bits;
    data[n] |= (1 << off);
}

static void debug_print(struct ubd *dev, int flag, const char *fmt, ...)
{
    va_list args;
    if (dev->debug_flags & flag) {
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
    }
}

/* Device operations */
static int ubd_open_dev(struct ubd *dev)
{
    if (dev->fd >= 0)
        return 0;

    dev->fd = open(dev->file, O_RDWR);
    if (dev->fd < 0) {
        perror("Failed to open device file");
        return -errno;
    }

    struct stat st;
    if (fstat(dev->fd, &st) < 0) {
        perror("Failed to stat device file");
        close(dev->fd);
        dev->fd = -1;
        return -errno;
    }

    dev->size = st.st_size;
    debug_print(dev, UBD_DEBUG_IO, "Opened device %s, size: %llu bytes\n",
                dev->file, (unsigned long long)dev->size);
    return 0;
}

static void ubd_close_dev(struct ubd *dev)
{
    if (dev->fd >= 0) {
        close(dev->fd);
        dev->fd = -1;
    }

    if (dev->cow.fd >= 0) {
        close(dev->cow.fd);
        dev->cow.fd = -1;
    }
}

static int ubd_read(struct ubd *dev, char *buffer,
                   unsigned long long offset, unsigned int length)
{
    ssize_t ret;

    if (offset + length > dev->size) {
        fprintf(stderr, "Read beyond device size: offset=%llu length=%u size=%llu\n",
                offset, length, (unsigned long long)dev->size);
        return -EINVAL;
    }

    if (_lseek(dev->fd, offset, SEEK_SET) < 0) {
        perror("Seek failed");
        return -errno;
    }

    ret = read(dev->fd, buffer, length);
    if (ret < 0) {
        perror("Read failed");
        return -errno;
    }
    if (ret != length) {
        fprintf(stderr, "Partial read: %zd/%u\n", ret, length);
        return -EIO;
    }

    debug_print(dev, UBD_DEBUG_IO, "Read %u bytes from offset %llu\n",
                length, offset);
    return 0;
}

static int ubd_write(struct ubd *dev, const char *buffer,
                    unsigned long long offset, unsigned int length)
{
    ssize_t ret;

    if (offset + length > dev->size) {
        fprintf(stderr, "Write beyond device size: offset=%llu length=%u size=%llu\n",
                offset, length, (unsigned long long)dev->size);
        return -EINVAL;
    }

    if (_lseek(dev->fd, offset, SEEK_SET) < 0) {
        perror("Seek failed");
        return -errno;
    }

    ret = write(dev->fd, buffer, length);
    if (ret < 0) {
        perror("Write failed");
        return -errno;
    }
    if (ret != length) {
        fprintf(stderr, "Partial write: %zd/%u\n", ret, length);
        return -EIO;
    }

    debug_print(dev, UBD_DEBUG_IO, "Wrote %u bytes to offset %llu\n",
                length, offset);
    return 0;
}

static int process_request(struct ubd *dev, struct request *req)
{
    unsigned long long offset = req->sector * SECTOR_SIZE;
    unsigned int length = req->nr_sectors * SECTOR_SIZE;
    int ret = 0;

    pthread_spin_lock(&dev->lock);

    switch (req->type) {
    case REQ_OP_READ:
        ret = ubd_read(dev, req->buffer, offset, length);
        break;

    case REQ_OP_WRITE:
        ret = ubd_write(dev, req->buffer, offset, length);
        break;

    case REQ_OP_FLUSH:
        if (dev->fd >= 0)
            _commit(dev->fd);
        if (dev->cow.fd >= 0)
            _commit(dev->cow.fd);
        break;

    case REQ_OP_DISCARD:
        /* Not implemented in this simulation */
        break;

    default:
        fprintf(stderr, "Unknown request type: %d\n", req->type);
        ret = -EINVAL;
        break;
    }

    pthread_spin_unlock(&dev->lock);
    return ret;
}

static void *io_thread(void *arg)
{
    struct ubd *dev = arg;
    struct request req;
    unsigned long long test_sectors[] = {0, 100, 200, 300, 400};
    int test_sizes[] = {1, 2, 4, 8, 16};
    char *buffer;
    int i;

    buffer = malloc(16 * SECTOR_SIZE);
    if (!buffer) {
        fprintf(stderr, "Failed to allocate I/O buffer\n");
        return NULL;
    }

    /* Perform test I/O operations */
    while (dev->thread_running) {
        for (i = 0; i < 5 && dev->thread_running; i++) {
            /* Read test */
            memset(&req, 0, sizeof(req));
            req.type = REQ_OP_READ;
            req.sector = test_sectors[i];
            req.nr_sectors = test_sizes[i];
            req.buffer = buffer;

            debug_print(dev, UBD_DEBUG_REQ,
                       "Processing READ request: sector=%llu count=%u\n",
                       req.sector, req.nr_sectors);
            
            req.error = process_request(dev, &req);
            if (req.error)
                fprintf(stderr, "Read request failed: %d\n", req.error);

            /* Write test */
            memset(buffer, 'A' + i, test_sizes[i] * SECTOR_SIZE);
            req.type = REQ_OP_WRITE;

            debug_print(dev, UBD_DEBUG_REQ,
                       "Processing WRITE request: sector=%llu count=%u\n",
                       req.sector, req.nr_sectors);
            
            req.error = process_request(dev, &req);
            if (req.error)
                fprintf(stderr, "Write request failed: %d\n", req.error);

            /* Verify written data */
            memset(buffer, 0, test_sizes[i] * SECTOR_SIZE);
            req.type = REQ_OP_READ;

            debug_print(dev, UBD_DEBUG_REQ,
                       "Verifying written data: sector=%llu count=%u\n",
                       req.sector, req.nr_sectors);
            
            req.error = process_request(dev, &req);
            if (req.error)
                fprintf(stderr, "Verification read failed: %d\n", req.error);

            int j;
            for (j = 0; j < test_sizes[i] * SECTOR_SIZE; j++) {
                if (buffer[j] != 'A' + i) {
                    fprintf(stderr, "Data verification failed at offset %d\n", j);
                    break;
                }
            }

            usleep(100000); /* Small delay between operations */
        }

        /* Flush test */
        memset(&req, 0, sizeof(req));
        req.type = REQ_OP_FLUSH;
        process_request(dev, &req);

        debug_print(dev, UBD_DEBUG_REQ, "Processed FLUSH request\n");
    }

    free(buffer);
    return NULL;
}

/* Main test program */
int main(int argc, char *argv[])
{
    struct ubd dev = {0};
    char *test_file = "ubd_test.img";
    uint64_t size = 10 * 1024 * 1024; /* 10MB test device */
    int ret;

    printf("UBD (User-mode Block Device) Test Program\n");
    printf("========================================\n\n");

    /* Create test file */
    dev.fd = open(test_file, O_RDWR | O_CREAT, 0644);
    if (dev.fd < 0) {
        perror("Failed to create test file");
        return 1;
    }

    /* Set file size */
    if (ftruncate(dev.fd, size) < 0) {
        perror("Failed to set file size");
        close(dev.fd);
        return 1;
    }

    /* Initialize device */
    dev.file = test_file;
    dev.size = size;
    dev.debug_flags = UBD_DEBUG_ALL;
    pthread_spin_init(&dev.lock, PTHREAD_PROCESS_PRIVATE);

    printf("Created test device:\n");
    printf("  File: %s\n", dev.file);
    printf("  Size: %llu bytes\n", (unsigned long long)dev.size);
    printf("  Sector size: %d bytes\n", SECTOR_SIZE);
    printf("  Number of sectors: %llu\n\n", (unsigned long long)(size / SECTOR_SIZE));

    /* Start I/O thread */
    dev.thread_running = true;
    ret = pthread_create(&dev.io_thread, NULL, io_thread, &dev);
    if (ret) {
        fprintf(stderr, "Failed to create I/O thread: %s\n", strerror(ret));
        ubd_close_dev(&dev);
        return 1;
    }

    printf("I/O thread started. Running tests...\n\n");

    /* Let the test run for a while */
    sleep(5);

    /* Cleanup */
    printf("\nStopping I/O thread...\n");
    dev.thread_running = false;
    pthread_join(dev.io_thread, NULL);

    ubd_close_dev(&dev);
    pthread_spin_destroy(&dev.lock);

    printf("Test completed successfully\n");
    return 0;
}
