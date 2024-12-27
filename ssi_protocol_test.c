/*
 * SSI Protocol Test Program
 * This is a standalone simulation of the SSI (Synchronous Serial Interface) protocol
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

/* Protocol Constants */
#define SSIP_MAX_MTU         65535
#define SSIP_DEFAULT_MTU     4000
#define SSIP_TXQUEUE_LEN     100
#define SSIP_WDTOUT          2000    /* Watchdog timeout in ms */
#define SSIP_KATOUT          15      /* Keep-alive timeout in ms */
#define SSIP_MAX_CMDS        5       /* Number of pre-allocated command buffers */

/* Command Definitions */
#define SSIP_COMMAND(data)   ((data) >> 28)
#define SSIP_PAYLOAD(data)   ((data) & 0xfffffff)

/* Commands */
#define SSIP_SW_BREAK        0
#define SSIP_BOOTINFO_REQ    1
#define SSIP_BOOTINFO_RESP   2
#define SSIP_WAKETEST_RESULT 3
#define SSIP_START_TRANS     4
#define SSIP_READY           5

/* Payloads */
#define SSIP_DATA_VERSION(data)  ((data) & 0xff)
#define SSIP_LOCAL_VERID    1
#define SSIP_WAKETEST_OK    0
#define SSIP_WAKETEST_FAILED 1
#define SSIP_PDU_LENGTH(data) (((data) >> 8) & 0xffff)
#define SSIP_MSG_ID(data)    ((data) & 0xff)

/* Generic Command */
#define SSIP_CMD(cmd, payload) (((cmd) << 28) | ((payload) & 0xfffffff))

/* Command Constructors */
#define SSIP_BOOTINFO_REQ_CMD(ver) \
        SSIP_CMD(SSIP_BOOTINFO_REQ, SSIP_DATA_VERSION(ver))
#define SSIP_BOOTINFO_RESP_CMD(ver) \
        SSIP_CMD(SSIP_BOOTINFO_RESP, SSIP_DATA_VERSION(ver))
#define SSIP_START_TRANS_CMD(pdulen, id) \
        SSIP_CMD(SSIP_START_TRANS, (((pdulen) << 8) | SSIP_MSG_ID(id)))
#define SSIP_READY_CMD       SSIP_CMD(SSIP_READY, 0)
#define SSIP_SWBREAK_CMD     SSIP_CMD(SSIP_SW_BREAK, 0)

/* State Machine States */
enum main_state {
    INIT,
    HANDSHAKE,
    ACTIVE
};

enum send_state {
    SEND_IDLE,
    WAIT4READY,
    SEND_READY,
    SENDING,
    SENDING_SWBREAK
};

enum recv_state {
    RECV_IDLE,
    RECV_READY,
    RECEIVING
};

/* Message Structure */
struct ssi_msg {
    uint32_t cmd;           /* Command or data */
    void *data;             /* Message payload */
    size_t len;            /* Payload length */
    struct ssi_msg *next;   /* Next message in queue */
    void (*complete)(struct ssi_msg *msg); /* Completion callback */
};

/* Protocol Context Structure */
struct ssi_protocol {
    /* State machines */
    enum main_state main_state;
    enum send_state send_state;
    enum recv_state recv_state;
    
    /* Protocol parameters */
    uint8_t rx_id;
    uint8_t tx_id;
    unsigned int tx_queue_len;
    
    /* Queues */
    struct ssi_msg *tx_queue;
    struct ssi_msg *rx_queue;
    struct ssi_msg *cmd_pool;
    
    /* Synchronization */
    pthread_mutex_t lock;
    pthread_cond_t tx_cond;
    pthread_cond_t rx_cond;
    
    /* Thread handles */
    pthread_t tx_thread;
    pthread_t rx_thread;
    pthread_t watchdog_thread;
    
    /* Status flags */
    bool running;
    bool error;
    
    /* Statistics */
    uint32_t tx_packets;
    uint32_t rx_packets;
    uint32_t tx_bytes;
    uint32_t rx_bytes;
    uint32_t errors;
};

/* Function Declarations */
static struct ssi_protocol *ssi_init(void);
static void ssi_cleanup(struct ssi_protocol *ssi);
static struct ssi_msg *ssi_alloc_msg(uint32_t cmd, void *data, size_t len);
static void ssi_free_msg(struct ssi_msg *msg);
static int ssi_queue_msg(struct ssi_protocol *ssi, struct ssi_msg *msg);
static struct ssi_msg *ssi_dequeue_msg(struct ssi_protocol *ssi);
static void *ssi_tx_thread(void *arg);
static void *ssi_rx_thread(void *arg);
static void *ssi_watchdog_thread(void *arg);
static void ssi_handle_command(struct ssi_protocol *ssi, uint32_t cmd);
static void ssi_send_command(struct ssi_protocol *ssi, uint32_t cmd);
static void ssi_dump_stats(struct ssi_protocol *ssi);

/* Initialize SSI Protocol */
static struct ssi_protocol *ssi_init(void) {
    struct ssi_protocol *ssi = calloc(1, sizeof(*ssi));
    if (!ssi)
        return NULL;
    
    /* Initialize state machines */
    ssi->main_state = INIT;
    ssi->send_state = SEND_IDLE;
    ssi->recv_state = RECV_IDLE;
    
    /* Initialize protocol parameters */
    ssi->tx_id = 0;
    ssi->rx_id = 0;
    ssi->tx_queue_len = 0;
    
    /* Initialize synchronization primitives */
    pthread_mutex_init(&ssi->lock, NULL);
    pthread_cond_init(&ssi->tx_cond, NULL);
    pthread_cond_init(&ssi->rx_cond, NULL);
    
    /* Set running flag */
    ssi->running = true;
    
    /* Start threads */
    pthread_create(&ssi->tx_thread, NULL, ssi_tx_thread, ssi);
    pthread_create(&ssi->rx_thread, NULL, ssi_rx_thread, ssi);
    pthread_create(&ssi->watchdog_thread, NULL, ssi_watchdog_thread, ssi);
    
    return ssi;
}

/* Cleanup SSI Protocol */
static void ssi_cleanup(struct ssi_protocol *ssi) {
    if (!ssi)
        return;
    
    /* Stop threads */
    ssi->running = false;
    pthread_cond_broadcast(&ssi->tx_cond);
    pthread_cond_broadcast(&ssi->rx_cond);
    
    /* Wait for threads to finish */
    pthread_join(ssi->tx_thread, NULL);
    pthread_join(ssi->rx_thread, NULL);
    pthread_join(ssi->watchdog_thread, NULL);
    
    /* Free queued messages */
    struct ssi_msg *msg;
    while ((msg = ssi->tx_queue)) {
        ssi->tx_queue = msg->next;
        ssi_free_msg(msg);
    }
    while ((msg = ssi->rx_queue)) {
        ssi->rx_queue = msg->next;
        ssi_free_msg(msg);
    }
    
    /* Cleanup synchronization primitives */
    pthread_mutex_destroy(&ssi->lock);
    pthread_cond_destroy(&ssi->tx_cond);
    pthread_cond_destroy(&ssi->rx_cond);
    
    free(ssi);
}

/* Allocate Message */
static struct ssi_msg *ssi_alloc_msg(uint32_t cmd, void *data, size_t len) {
    struct ssi_msg *msg = calloc(1, sizeof(*msg));
    if (!msg)
        return NULL;
    
    msg->cmd = cmd;
    if (len > 0 && data) {
        msg->data = malloc(len);
        if (!msg->data) {
            free(msg);
            return NULL;
        }
        memcpy(msg->data, data, len);
        msg->len = len;
    }
    
    return msg;
}

/* Free Message */
static void ssi_free_msg(struct ssi_msg *msg) {
    if (!msg)
        return;
    free(msg->data);
    free(msg);
}

/* Queue Message */
static int ssi_queue_msg(struct ssi_protocol *ssi, struct ssi_msg *msg) {
    pthread_mutex_lock(&ssi->lock);
    
    if (ssi->tx_queue_len >= SSIP_TXQUEUE_LEN) {
        pthread_mutex_unlock(&ssi->lock);
        return -ENOSPC;
    }
    
    /* Add to queue */
    msg->next = NULL;
    if (!ssi->tx_queue) {
        ssi->tx_queue = msg;
    } else {
        struct ssi_msg *last = ssi->tx_queue;
        while (last->next)
            last = last->next;
        last->next = msg;
    }
    ssi->tx_queue_len++;
    
    /* Signal transmitter */
    pthread_cond_signal(&ssi->tx_cond);
    pthread_mutex_unlock(&ssi->lock);
    
    return 0;
}

/* Dequeue Message */
static struct ssi_msg *ssi_dequeue_msg(struct ssi_protocol *ssi) {
    pthread_mutex_lock(&ssi->lock);
    
    struct ssi_msg *msg = ssi->tx_queue;
    if (msg) {
        ssi->tx_queue = msg->next;
        ssi->tx_queue_len--;
    }
    
    pthread_mutex_unlock(&ssi->lock);
    return msg;
}

/* Handle Received Command */
static void ssi_handle_command(struct ssi_protocol *ssi, uint32_t cmd) {
    uint32_t command = SSIP_COMMAND(cmd);
    uint32_t payload = SSIP_PAYLOAD(cmd);
    
    switch (command) {
    case SSIP_BOOTINFO_REQ:
        printf("Received BOOTINFO_REQ, version: %d\n", SSIP_DATA_VERSION(payload));
        ssi_send_command(ssi, SSIP_BOOTINFO_RESP_CMD(SSIP_LOCAL_VERID));
        break;
        
    case SSIP_BOOTINFO_RESP:
        printf("Received BOOTINFO_RESP, version: %d\n", SSIP_DATA_VERSION(payload));
        if (ssi->main_state == HANDSHAKE)
            ssi->main_state = ACTIVE;
        break;
        
    case SSIP_START_TRANS:
        printf("Received START_TRANS, length: %d, id: %d\n",
               SSIP_PDU_LENGTH(payload), SSIP_MSG_ID(payload));
        ssi_send_command(ssi, SSIP_READY_CMD);
        break;
        
    case SSIP_READY:
        printf("Received READY command\n");
        break;
        
    case SSIP_SW_BREAK:
        printf("Received SW_BREAK command\n");
        break;
        
    default:
        printf("Received unknown command: 0x%08x\n", cmd);
        break;
    }
}

/* Send Command */
static void ssi_send_command(struct ssi_protocol *ssi, uint32_t cmd) {
    struct ssi_msg *msg = ssi_alloc_msg(cmd, NULL, 0);
    if (msg)
        ssi_queue_msg(ssi, msg);
}

/* Transmit Thread */
static void *ssi_tx_thread(void *arg) {
    struct ssi_protocol *ssi = arg;
    struct ssi_msg *msg;
    
    while (ssi->running) {
        pthread_mutex_lock(&ssi->lock);
        while (ssi->running && !ssi->tx_queue)
            pthread_cond_wait(&ssi->tx_cond, &ssi->lock);
        pthread_mutex_unlock(&ssi->lock);
        
        if (!ssi->running)
            break;
        
        msg = ssi_dequeue_msg(ssi);
        if (msg) {
            /* Simulate transmission */
            usleep(1000); /* 1ms delay */
            
            printf("TX: Command 0x%08x\n", msg->cmd);
            ssi->tx_packets++;
            ssi->tx_bytes += msg->len;
            
            if (msg->complete)
                msg->complete(msg);
            else
                ssi_free_msg(msg);
        }
    }
    
    return NULL;
}

/* Receive Thread */
static void *ssi_rx_thread(void *arg) {
    struct ssi_protocol *ssi = arg;
    
    while (ssi->running) {
        /* Simulate reception */
        usleep(10000); /* 10ms delay */
        
        if (ssi->main_state == INIT) {
            /* Start handshake */
            ssi_send_command(ssi, SSIP_BOOTINFO_REQ_CMD(SSIP_LOCAL_VERID));
            ssi->main_state = HANDSHAKE;
        }
        
        ssi->rx_packets++;
    }
    
    return NULL;
}

/* Watchdog Thread */
static void *ssi_watchdog_thread(void *arg) {
    struct ssi_protocol *ssi = arg;
    
    while (ssi->running) {
        usleep(SSIP_WDTOUT * 1000); /* Convert to microseconds */
        
        pthread_mutex_lock(&ssi->lock);
        if (ssi->tx_queue_len > 0) {
            printf("Watchdog: %d messages in TX queue\n", ssi->tx_queue_len);
        }
        pthread_mutex_unlock(&ssi->lock);
    }
    
    return NULL;
}

/* Dump Statistics */
static void ssi_dump_stats(struct ssi_protocol *ssi) {
    printf("\nSSI Protocol Statistics:\n");
    printf("=======================\n");
    printf("Main State: %d\n", ssi->main_state);
    printf("Send State: %d\n", ssi->send_state);
    printf("Receive State: %d\n", ssi->recv_state);
    printf("TX Packets: %u\n", ssi->tx_packets);
    printf("RX Packets: %u\n", ssi->rx_packets);
    printf("TX Bytes: %u\n", ssi->tx_bytes);
    printf("RX Bytes: %u\n", ssi->rx_bytes);
    printf("Errors: %u\n", ssi->errors);
    printf("Queue Length: %d\n", ssi->tx_queue_len);
}

/* Test Functions */
static void test_basic_protocol(struct ssi_protocol *ssi) {
    printf("\nTesting basic protocol operation...\n");
    
    /* Wait for handshake to complete */
    sleep(1);
    
    /* Send some test commands */
    ssi_send_command(ssi, SSIP_START_TRANS_CMD(1024, 1));
    usleep(100000);
    ssi_send_command(ssi, SSIP_READY_CMD);
    usleep(100000);
    
    /* Send some test data */
    char test_data[] = "Hello, SSI Protocol!";
    struct ssi_msg *msg = ssi_alloc_msg(0, test_data, strlen(test_data));
    if (msg)
        ssi_queue_msg(ssi, msg);
    
    /* Wait for processing */
    sleep(1);
}

static void test_error_handling(struct ssi_protocol *ssi) {
    printf("\nTesting error handling...\n");
    
    /* Test queue overflow */
    for (int i = 0; i < SSIP_TXQUEUE_LEN + 5; i++) {
        char data[32];
        snprintf(data, sizeof(data), "Test message %d", i);
        struct ssi_msg *msg = ssi_alloc_msg(0, data, strlen(data));
        if (msg) {
            int ret = ssi_queue_msg(ssi, msg);
            if (ret < 0) {
                printf("Queue overflow at message %d\n", i);
                ssi_free_msg(msg);
                break;
            }
        }
    }
    
    /* Wait for queue to drain */
    sleep(2);
}

int main(void) {
    printf("SSI Protocol Test Program\n");
    printf("========================\n\n");
    
    /* Initialize protocol */
    struct ssi_protocol *ssi = ssi_init();
    if (!ssi) {
        printf("Failed to initialize SSI protocol\n");
        return 1;
    }
    
    /* Run tests */
    test_basic_protocol(ssi);
    test_error_handling(ssi);
    
    /* Display statistics */
    ssi_dump_stats(ssi);
    
    /* Cleanup */
    ssi_cleanup(ssi);
    
    printf("\nTest completed successfully!\n");
    return 0;
}
