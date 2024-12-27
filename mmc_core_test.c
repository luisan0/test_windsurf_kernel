/*
 * MMC Core Test Program
 * This is a standalone simulation of MMC core operations
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

/* MMC command definitions */
#define MMC_GO_IDLE_STATE         0
#define MMC_SEND_OP_COND         1
#define MMC_ALL_SEND_CID         2
#define MMC_SET_RELATIVE_ADDR    3
#define MMC_SWITCH              6
#define MMC_SELECT_CARD          7
#define MMC_SEND_EXT_CSD        8
#define MMC_SEND_CSD            9
#define MMC_SEND_CID           10
#define MMC_STOP_TRANSMISSION  12
#define MMC_SEND_STATUS        13
#define MMC_READ_SINGLE_BLOCK  17
#define MMC_READ_MULTIPLE_BLOCK 18
#define MMC_WRITE_BLOCK        24
#define MMC_WRITE_MULTIPLE_BLOCK 25

/* Response types */
#define MMC_RSP_PRESENT    (1 << 0)
#define MMC_RSP_136        (1 << 1)
#define MMC_RSP_CRC        (1 << 2)
#define MMC_RSP_BUSY       (1 << 3)
#define MMC_RSP_OPCODE     (1 << 4)

#define MMC_RSP_NONE       (0)
#define MMC_RSP_R1         (MMC_RSP_PRESENT|MMC_RSP_CRC|MMC_RSP_OPCODE)
#define MMC_RSP_R1B        (MMC_RSP_PRESENT|MMC_RSP_CRC|MMC_RSP_OPCODE|MMC_RSP_BUSY)
#define MMC_RSP_R2         (MMC_RSP_PRESENT|MMC_RSP_136|MMC_RSP_CRC)
#define MMC_RSP_R3         (MMC_RSP_PRESENT)
#define MMC_RSP_R4         (MMC_RSP_PRESENT)
#define MMC_RSP_R5         (MMC_RSP_PRESENT|MMC_RSP_CRC|MMC_RSP_OPCODE)
#define MMC_RSP_R6         (MMC_RSP_PRESENT|MMC_RSP_CRC|MMC_RSP_OPCODE)
#define MMC_RSP_R7         (MMC_RSP_PRESENT|MMC_RSP_CRC|MMC_RSP_OPCODE)

/* Card states */
#define MMC_STATE_PRESENT     (1 << 0)
#define MMC_STATE_READONLY    (1 << 1)
#define MMC_STATE_HIGHSPEED   (1 << 2)
#define MMC_STATE_BLOCKADDR   (1 << 3)
#define MMC_STATE_HIGHCAP     (1 << 4)
#define MMC_STATE_ULTRAHIGHSPEED (1 << 5)
#define MMC_STATE_DDR         (1 << 6)
#define MMC_STATE_SUSPENDED   (1 << 7)

/* Error codes */
#define MMC_ERR_NONE          0
#define MMC_ERR_TIMEOUT       1
#define MMC_ERR_CRC           2
#define MMC_ERR_INVALID       3
#define MMC_ERR_FAILED        4
#define MMC_ERR_RETRY         5

/* Command flags */
#define MMC_CMD_MASK         (3 << 5)
#define MMC_CMD_AC           (0 << 5)
#define MMC_CMD_ADTC         (1 << 5)
#define MMC_CMD_BC           (2 << 5)
#define MMC_CMD_BCR          (3 << 5)

/* Maximum limits */
#define MMC_MAX_RETRIES      5
#define MMC_MAX_COMMANDS     60
#define MMC_MAX_CARDS        10
#define MMC_MAX_SEGMENTS     128

/* Command structure */
struct mmc_command {
    uint32_t opcode;
    uint32_t arg;
    uint32_t resp[4];
    uint32_t flags;
    uint32_t retries;
    int error;
    void *data;
    size_t data_len;
};

/* Request structure */
struct mmc_request {
    struct mmc_command cmd;
    struct mmc_command stop;
    int error;
    bool need_stop;
    void (*done)(struct mmc_request *);
    void *context;
};

/* Host structure */
struct mmc_host {
    int id;
    char name[32];
    uint32_t caps;
    uint32_t max_seg_size;
    uint32_t max_segs;
    uint32_t max_req_size;
    uint32_t max_blk_size;
    uint32_t max_blk_count;
    uint32_t clock;
    uint32_t voltage;
    
    /* Current request */
    struct mmc_request *mrq;
    
    /* Simulated hardware state */
    bool powered;
    bool bus_active;
    uint32_t state;
    
    /* Statistics */
    uint64_t commands;
    uint64_t errors;
    uint64_t timeouts;
    uint64_t retries;
    uint64_t bytes_xfered;
    
    /* Lock */
    pthread_mutex_t lock;
};

/* Card structure */
struct mmc_card {
    struct mmc_host *host;
    uint32_t rca;
    uint32_t type;
    uint32_t state;
    uint8_t cid[16];
    uint8_t csd[16];
    uint8_t ext_csd[512];
    uint32_t raw_cid[4];
    uint32_t raw_csd[4];
    
    uint32_t clock;
    uint32_t voltage;
    bool removed;
    bool present;
};

/* Global variables */
static struct mmc_host *mmc_hosts[MMC_MAX_CARDS];
static int mmc_host_count = 0;
static pthread_mutex_t mmc_lock = PTHREAD_MUTEX_INITIALIZER;

/* Function declarations */
static struct mmc_host *mmc_alloc_host(void);
static void mmc_free_host(struct mmc_host *host);
static int mmc_add_host(struct mmc_host *host);
static void mmc_remove_host(struct mmc_host *host);
static struct mmc_request *mmc_alloc_request(void);
static void mmc_free_request(struct mmc_request *mrq);
static int mmc_wait_for_req(struct mmc_host *host, struct mmc_request *mrq);
static void mmc_request_done(struct mmc_host *host, struct mmc_request *mrq);
static bool mmc_should_fail_request(struct mmc_host *host, struct mmc_request *mrq);
static void mmc_simulate_command(struct mmc_host *host, struct mmc_command *cmd);
static void mmc_dump_status(struct mmc_host *host);

/* Allocate host */
static struct mmc_host *mmc_alloc_host(void) {
    struct mmc_host *host;
    
    host = calloc(1, sizeof(*host));
    if (!host)
        return NULL;
    
    /* Initialize host parameters */
    host->max_seg_size = 65536;
    host->max_segs = MMC_MAX_SEGMENTS;
    host->max_req_size = 524288;
    host->max_blk_size = 512;
    host->max_blk_count = 256;
    host->clock = 50000000;  /* 50MHz */
    host->voltage = 0x00FF8080;  /* Typical voltage ranges */
    
    pthread_mutex_init(&host->lock, NULL);
    
    return host;
}

/* Free host */
static void mmc_free_host(struct mmc_host *host) {
    if (!host)
        return;
    
    pthread_mutex_destroy(&host->lock);
    free(host);
}

/* Add host */
static int mmc_add_host(struct mmc_host *host) {
    pthread_mutex_lock(&mmc_lock);
    
    if (mmc_host_count >= MMC_MAX_CARDS) {
        pthread_mutex_unlock(&mmc_lock);
        return -ENOSPC;
    }
    
    /* Find free slot */
    int id;
    for (id = 0; id < MMC_MAX_CARDS; id++) {
        if (!mmc_hosts[id])
            break;
    }
    
    /* Initialize host */
    host->id = id;
    snprintf(host->name, sizeof(host->name), "mmc%d", id);
    host->powered = true;
    host->state = MMC_STATE_PRESENT;
    
    /* Add to array */
    mmc_hosts[id] = host;
    mmc_host_count++;
    
    pthread_mutex_unlock(&mmc_lock);
    return 0;
}

/* Remove host */
static void mmc_remove_host(struct mmc_host *host) {
    if (!host)
        return;
    
    pthread_mutex_lock(&mmc_lock);
    
    if (mmc_hosts[host->id] == host) {
        mmc_hosts[host->id] = NULL;
        mmc_host_count--;
    }
    
    pthread_mutex_unlock(&mmc_lock);
    
    mmc_free_host(host);
}

/* Allocate request */
static struct mmc_request *mmc_alloc_request(void) {
    return calloc(1, sizeof(struct mmc_request));
}

/* Free request */
static void mmc_free_request(struct mmc_request *mrq) {
    if (!mrq)
        return;
    
    free(mrq->cmd.data);
    free(mrq);
}

/* Check if request should fail */
static bool mmc_should_fail_request(struct mmc_host *host, struct mmc_request *mrq) {
    /* Simulate random failures */
    if (rand() % 100 < 5) {  /* 5% failure rate */
        mrq->error = MMC_ERR_FAILED;
        return true;
    }
    
    /* Check for invalid commands */
    if (mrq->cmd.opcode >= MMC_MAX_COMMANDS) {
        mrq->error = MMC_ERR_INVALID;
        return true;
    }
    
    /* Check host state */
    if (!host->powered) {
        mrq->error = MMC_ERR_FAILED;
        return true;
    }
    
    /* Check for timeouts */
    if (rand() % 100 < 2) {  /* 2% timeout rate */
        mrq->error = MMC_ERR_TIMEOUT;
        return true;
    }
    
    return false;
}

/* Simulate command execution */
static void mmc_simulate_command(struct mmc_host *host, struct mmc_command *cmd) {
    /* Simulate command processing delay */
    usleep(1000 + (rand() % 1000));
    
    switch (cmd->opcode) {
    case MMC_GO_IDLE_STATE:
        cmd->resp[0] = 0;
        break;
        
    case MMC_SEND_OP_COND:
        cmd->resp[0] = 0x80FF8000;  /* Card powered up and ready */
        break;
        
    case MMC_ALL_SEND_CID:
        /* Simulate CID response */
        cmd->resp[0] = 0x11223344;
        cmd->resp[1] = 0x55667788;
        cmd->resp[2] = 0x99AABBCC;
        cmd->resp[3] = 0xDDEEFF00;
        break;
        
    case MMC_SEND_STATUS:
        cmd->resp[0] = 0x00000900;  /* Ready for data state */
        break;
        
    case MMC_READ_SINGLE_BLOCK:
    case MMC_WRITE_BLOCK:
        if (cmd->data && cmd->data_len > 0) {
            /* Simulate data transfer */
            host->bytes_xfered += cmd->data_len;
        }
        cmd->resp[0] = 0x00000900;  /* Success */
        break;
        
    default:
        cmd->resp[0] = 0x00000900;  /* Generic success response */
        break;
    }
    
    host->commands++;
}

/* Wait for request completion */
static int mmc_wait_for_req(struct mmc_host *host, struct mmc_request *mrq) {
    pthread_mutex_lock(&host->lock);
    
    /* Check if request should fail */
    if (mmc_should_fail_request(host, mrq)) {
        host->errors++;
        pthread_mutex_unlock(&host->lock);
        return mrq->error;
    }
    
    /* Process command */
    mmc_simulate_command(host, &mrq->cmd);
    
    /* Process stop command if needed */
    if (mrq->need_stop)
        mmc_simulate_command(host, &mrq->stop);
    
    /* Complete request */
    mmc_request_done(host, mrq);
    
    pthread_mutex_unlock(&host->lock);
    return MMC_ERR_NONE;
}

/* Complete request */
static void mmc_request_done(struct mmc_host *host, struct mmc_request *mrq) {
    if (mrq->done)
        mrq->done(mrq);
}

/* Dump host status */
static void mmc_dump_status(struct mmc_host *host) {
    printf("\nMMC Host Status (%s):\n", host->name);
    printf("===================\n");
    printf("Power state: %s\n", host->powered ? "on" : "off");
    printf("Bus state: %s\n", host->bus_active ? "active" : "inactive");
    printf("Clock: %u Hz\n", host->clock);
    printf("Commands executed: %lu\n", host->commands);
    printf("Errors: %lu\n", host->errors);
    printf("Timeouts: %lu\n", host->timeouts);
    printf("Retries: %lu\n", host->retries);
    printf("Bytes transferred: %lu\n", host->bytes_xfered);
}

/* Test functions */
static void test_basic_commands(struct mmc_host *host) {
    printf("\nTesting basic commands...\n");
    
    /* Test GO_IDLE_STATE */
    struct mmc_request *mrq = mmc_alloc_request();
    if (!mrq) {
        printf("Failed to allocate request\n");
        return;
    }
    
    mrq->cmd.opcode = MMC_GO_IDLE_STATE;
    mrq->cmd.flags = MMC_RSP_NONE;
    
    printf("Sending GO_IDLE_STATE command...\n");
    int ret = mmc_wait_for_req(host, mrq);
    if (ret)
        printf("Command failed: %d\n", ret);
    else
        printf("Command successful\n");
    
    mmc_free_request(mrq);
    
    /* Test SEND_STATUS */
    mrq = mmc_alloc_request();
    if (!mrq) {
        printf("Failed to allocate request\n");
        return;
    }
    
    mrq->cmd.opcode = MMC_SEND_STATUS;
    mrq->cmd.flags = MMC_RSP_R1;
    
    printf("Sending SEND_STATUS command...\n");
    ret = mmc_wait_for_req(host, mrq);
    if (ret)
        printf("Command failed: %d\n", ret);
    else
        printf("Card status: 0x%08x\n", mrq->cmd.resp[0]);
    
    mmc_free_request(mrq);
}

static void test_data_transfer(struct mmc_host *host) {
    printf("\nTesting data transfer...\n");
    
    /* Prepare test data */
    uint8_t *data = malloc(512);
    if (!data) {
        printf("Failed to allocate data buffer\n");
        return;
    }
    
    for (int i = 0; i < 512; i++)
        data[i] = i & 0xff;
    
    /* Test WRITE_BLOCK */
    struct mmc_request *mrq = mmc_alloc_request();
    if (!mrq) {
        printf("Failed to allocate request\n");
        free(data);
        return;
    }
    
    mrq->cmd.opcode = MMC_WRITE_BLOCK;
    mrq->cmd.flags = MMC_RSP_R1 | MMC_CMD_ADTC;
    mrq->cmd.arg = 0;  /* Block 0 */
    mrq->cmd.data = data;
    mrq->cmd.data_len = 512;
    
    printf("Writing 512 bytes to block 0...\n");
    int ret = mmc_wait_for_req(host, mrq);
    if (ret)
        printf("Write failed: %d\n", ret);
    else
        printf("Write successful\n");
    
    mmc_free_request(mrq);
    
    /* Test READ_SINGLE_BLOCK */
    mrq = mmc_alloc_request();
    if (!mrq) {
        printf("Failed to allocate request\n");
        free(data);
        return;
    }
    
    mrq->cmd.opcode = MMC_READ_SINGLE_BLOCK;
    mrq->cmd.flags = MMC_RSP_R1 | MMC_CMD_ADTC;
    mrq->cmd.arg = 0;  /* Block 0 */
    mrq->cmd.data = malloc(512);
    if (!mrq->cmd.data) {
        printf("Failed to allocate read buffer\n");
        mmc_free_request(mrq);
        free(data);
        return;
    }
    mrq->cmd.data_len = 512;
    
    printf("Reading 512 bytes from block 0...\n");
    ret = mmc_wait_for_req(host, mrq);
    if (ret)
        printf("Read failed: %d\n", ret);
    else
        printf("Read successful\n");
    
    mmc_free_request(mrq);
    free(data);
}

static void test_error_handling(struct mmc_host *host) {
    printf("\nTesting error handling...\n");
    
    /* Test invalid command */
    struct mmc_request *mrq = mmc_alloc_request();
    if (!mrq) {
        printf("Failed to allocate request\n");
        return;
    }
    
    mrq->cmd.opcode = MMC_MAX_COMMANDS + 1;
    printf("Sending invalid command...\n");
    int ret = mmc_wait_for_req(host, mrq);
    if (ret)
        printf("Command failed as expected: %d\n", ret);
    else
        printf("Command unexpectedly succeeded\n");
    
    mmc_free_request(mrq);
    
    /* Test with powered down host */
    host->powered = false;
    
    mrq = mmc_alloc_request();
    if (!mrq) {
        printf("Failed to allocate request\n");
        return;
    }
    
    mrq->cmd.opcode = MMC_SEND_STATUS;
    printf("Sending command to powered-down host...\n");
    ret = mmc_wait_for_req(host, mrq);
    if (ret)
        printf("Command failed as expected: %d\n", ret);
    else
        printf("Command unexpectedly succeeded\n");
    
    mmc_free_request(mrq);
    host->powered = true;
}

int main(void) {
    printf("MMC Core Test Program\n");
    printf("====================\n\n");
    
    /* Seed random number generator */
    srand(time(NULL));
    
    /* Create host */
    struct mmc_host *host = mmc_alloc_host();
    if (!host) {
        printf("Failed to allocate host\n");
        return 1;
    }
    
    /* Add host */
    int ret = mmc_add_host(host);
    if (ret < 0) {
        printf("Failed to add host: %d\n", ret);
        mmc_free_host(host);
        return 1;
    }
    
    printf("Created host %s\n", host->name);
    
    /* Run tests */
    test_basic_commands(host);
    test_data_transfer(host);
    test_error_handling(host);
    
    /* Display statistics */
    mmc_dump_status(host);
    
    /* Cleanup */
    mmc_remove_host(host);
    
    printf("\nTest completed successfully!\n");
    return 0;
}
