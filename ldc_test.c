/*
 * LDC (Logical Domain Channel) Test Program
 * This is a standalone simulation of the SPARC LDC functionality
 * Author: Cascade AI
 * Date: 2024-12-27
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>

/* Basic definitions */
#define LDC_PACKET_SIZE 64
#define PAGE_SIZE 4096
#define BITS_PER_LONG 64
#define LDC_DEFAULT_MTU (4 * LDC_PACKET_SIZE)
#define LDC_DEFAULT_NUM_ENTRIES (PAGE_SIZE / LDC_PACKET_SIZE)
#define LDC_IRQ_NAME_MAX 32
#define LDC_TIMEOUT_USEC 10000

/* Debug flags */
#define LDC_DEBUG_STATE    0x0001
#define LDC_DEBUG_QUEUE    0x0002
#define LDC_DEBUG_IRQ      0x0004
#define LDC_DEBUG_TX       0x0008
#define LDC_DEBUG_RX       0x0010
#define LDC_DEBUG_ALL      0xffff

/* Channel states */
#define LDC_STATE_INVALID    0x00
#define LDC_STATE_INIT       0x01
#define LDC_STATE_BOUND      0x02
#define LDC_STATE_READY      0x03
#define LDC_STATE_CONNECTED  0x04

/* Handshake states */
#define LDC_HS_CLOSED     0x00
#define LDC_HS_OPEN       0x01
#define LDC_HS_GOTVERS    0x02
#define LDC_HS_SENTRTR    0x03
#define LDC_HS_GOTRTR     0x04
#define LDC_HS_COMPLETE   0x10

/* Packet types and subtypes */
#define LDC_CTRL          0x01
#define LDC_DATA          0x02
#define LDC_ERR           0x10

#define LDC_INFO          0x01
#define LDC_ACK           0x02
#define LDC_NACK          0x04

/* Control commands */
#define LDC_VERS          0x01
#define LDC_RTS           0x02
#define LDC_RTR           0x03
#define LDC_RDX           0x04
#define LDC_CTRL_MSK      0x0f

/* Fragment flags */
#define LDC_LEN           0x3f
#define LDC_FRAG_MASK     0xc0
#define LDC_START         0x40
#define LDC_STOP          0x80

/* Channel flags */
#define LDC_FLAG_ALLOCED_QUEUES      0x01
#define LDC_FLAG_REGISTERED_QUEUES   0x02
#define LDC_FLAG_REGISTERED_IRQS     0x04
#define LDC_FLAG_RESET               0x10

/* Structures */
struct ldc_version {
    uint16_t major;
    uint16_t minor;
};

struct ldc_packet {
    uint8_t type;
    uint8_t stype;
    uint8_t ctrl;
    uint8_t env;
    uint32_t seqid;
    union {
        uint8_t u_data[LDC_PACKET_SIZE - 8];
        struct {
            uint32_t pad;
            uint32_t ackid;
            uint8_t r_data[LDC_PACKET_SIZE - 8 - 8];
        } r;
    } u;
};

struct ldc_channel_config {
    unsigned long mode;
    unsigned long debug;
};

struct ldc_channel {
    pthread_spinlock_t lock;
    unsigned long id;

    uint8_t *mssbuf;
    uint32_t mssbuf_len;
    uint32_t mssbuf_off;

    struct ldc_packet *tx_base;
    unsigned long tx_head;
    unsigned long tx_tail;
    unsigned long tx_num_entries;
    unsigned long tx_acked;

    struct ldc_packet *rx_base;
    unsigned long rx_head;
    unsigned long rx_tail;
    unsigned long rx_num_entries;

    uint32_t rcv_nxt;
    uint32_t snd_nxt;

    unsigned long chan_state;
    struct ldc_channel_config cfg;
    void *event_arg;

    struct ldc_version ver;

    uint8_t hs_state;
    uint8_t flags;
    uint8_t mss;
    uint8_t state;

    char rx_irq_name[LDC_IRQ_NAME_MAX];
    char tx_irq_name[LDC_IRQ_NAME_MAX];

    bool is_running;
    pthread_t rx_thread;
    pthread_t tx_thread;
};

/* Function declarations */
static const char *state_to_str(uint8_t state);
static unsigned long __advance(unsigned long off, unsigned long num_entries);
static unsigned long rx_advance(struct ldc_channel *lp, unsigned long off);
static unsigned long tx_advance(struct ldc_channel *lp, unsigned long off);
static bool rx_seq_ok(struct ldc_channel *lp, uint32_t seqid);
static void send_events(struct ldc_channel *lp, unsigned int event_mask);
static int process_control_frame(struct ldc_channel *lp, struct ldc_packet *p);
static int process_data_frame(struct ldc_channel *lp, struct ldc_packet *p);
static int process_error_frame(struct ldc_channel *lp, struct ldc_packet *p);

/* Helper functions */
static const char *state_to_str(uint8_t state)
{
    switch (state) {
    case LDC_STATE_INVALID:
        return "INVALID";
    case LDC_STATE_INIT:
        return "INIT";
    case LDC_STATE_BOUND:
        return "BOUND";
    case LDC_STATE_READY:
        return "READY";
    case LDC_STATE_CONNECTED:
        return "CONNECTED";
    default:
        return "<UNKNOWN>";
    }
}

static unsigned long __advance(unsigned long off, unsigned long num_entries)
{
    off++;
    if (off == num_entries)
        off = 0;
    return off;
}

static unsigned long rx_advance(struct ldc_channel *lp, unsigned long off)
{
    return __advance(off, lp->rx_num_entries);
}

static unsigned long tx_advance(struct ldc_channel *lp, unsigned long off)
{
    return __advance(off, lp->tx_num_entries);
}

static bool rx_seq_ok(struct ldc_channel *lp, uint32_t seqid)
{
    return seqid == lp->rcv_nxt;
}

/* Queue management */
static int alloc_queue(unsigned long num_entries,
                      struct ldc_packet **base)
{
    size_t size = num_entries * sizeof(struct ldc_packet);
    void *q = calloc(1, size);
    if (!q)
        return -1;
    *base = q;
    return 0;
}

static void free_queue(struct ldc_packet *q)
{
    free(q);
}

/* Channel operations */
static struct ldc_channel *ldc_alloc(unsigned long id,
                                   const struct ldc_channel_config *cfgp)
{
    struct ldc_channel *lp;

    lp = calloc(1, sizeof(*lp));
    if (!lp)
        return NULL;

    pthread_spin_init(&lp->lock, PTHREAD_PROCESS_PRIVATE);
    lp->id = id;
    lp->cfg = *cfgp;
    lp->state = LDC_STATE_INIT;
    lp->hs_state = LDC_HS_CLOSED;
    lp->ver.major = 1;
    lp->ver.minor = 0;

    snprintf(lp->rx_irq_name, LDC_IRQ_NAME_MAX, "ldc%lu-rx", id);
    snprintf(lp->tx_irq_name, LDC_IRQ_NAME_MAX, "ldc%lu-tx", id);

    return lp;
}

static void ldc_free(struct ldc_channel *lp)
{
    if (lp->flags & LDC_FLAG_ALLOCED_QUEUES) {
        free_queue(lp->rx_base);
        free_queue(lp->tx_base);
    }
    pthread_spin_destroy(&lp->lock);
    free(lp);
}

static int ldc_bind(struct ldc_channel *lp)
{
    int err;

    if (lp->state != LDC_STATE_INIT)
        return -1;

    if (!(lp->flags & LDC_FLAG_ALLOCED_QUEUES)) {
        err = alloc_queue(LDC_DEFAULT_NUM_ENTRIES, &lp->rx_base);
        if (err)
            return err;

        err = alloc_queue(LDC_DEFAULT_NUM_ENTRIES, &lp->tx_base);
        if (err) {
            free_queue(lp->rx_base);
            return err;
        }

        lp->rx_num_entries = LDC_DEFAULT_NUM_ENTRIES;
        lp->tx_num_entries = LDC_DEFAULT_NUM_ENTRIES;
        lp->flags |= LDC_FLAG_ALLOCED_QUEUES;
    }

    lp->tx_head = lp->tx_tail = 0;
    lp->rx_head = lp->rx_tail = 0;
    lp->rcv_nxt = 0;
    lp->snd_nxt = 0;

    lp->state = LDC_STATE_BOUND;
    return 0;
}

/* Packet processing thread functions */
static void *rx_thread(void *arg)
{
    struct ldc_channel *lp = arg;
    struct ldc_packet *p;

    while (lp->is_running) {
        pthread_spin_lock(&lp->lock);

        if (lp->rx_head != lp->rx_tail) {
            p = &lp->rx_base[lp->rx_tail];
            
            switch (p->type) {
            case LDC_CTRL:
                process_control_frame(lp, p);
                break;
            case LDC_DATA:
                process_data_frame(lp, p);
                break;
            case LDC_ERR:
                process_error_frame(lp, p);
                break;
            }

            lp->rx_tail = rx_advance(lp, lp->rx_tail);
        }

        pthread_spin_unlock(&lp->lock);
        usleep(1000);  // Small delay to prevent busy-waiting
    }

    return NULL;
}

static void *tx_thread(void *arg)
{
    struct ldc_channel *lp = arg;
    struct ldc_packet *p;

    while (lp->is_running) {
        pthread_spin_lock(&lp->lock);

        if (lp->tx_head != lp->tx_tail) {
            p = &lp->tx_base[lp->tx_tail];
            // Simulate packet transmission
            printf("TX: Channel %lu sending packet type=%d stype=%d ctrl=%d\n",
                   lp->id, p->type, p->stype, p->ctrl);
            lp->tx_tail = tx_advance(lp, lp->tx_tail);
        }

        pthread_spin_unlock(&lp->lock);
        usleep(1000);  // Small delay to prevent busy-waiting
    }

    return NULL;
}

/* Frame processing */
static int process_control_frame(struct ldc_channel *lp, struct ldc_packet *p)
{
    printf("RX: Channel %lu received control frame stype=%d ctrl=%d\n",
           lp->id, p->stype, p->ctrl);
    return 0;
}

static int process_data_frame(struct ldc_channel *lp, struct ldc_packet *p)
{
    if (!rx_seq_ok(lp, p->seqid))
        return -1;

    printf("RX: Channel %lu received data frame seqid=%u\n",
           lp->id, p->seqid);
    lp->rcv_nxt++;
    return 0;
}

static int process_error_frame(struct ldc_channel *lp, struct ldc_packet *p)
{
    printf("RX: Channel %lu received error frame\n", lp->id);
    return 0;
}

/* Test functions */
static void simulate_packet_exchange(struct ldc_channel *lp)
{
    struct ldc_packet *p;
    int i;

    // Simulate sending control packets
    pthread_spin_lock(&lp->lock);
    
    for (i = 0; i < 5; i++) {
        p = &lp->tx_base[lp->tx_head];
        memset(p, 0, sizeof(*p));
        
        p->type = LDC_CTRL;
        p->stype = LDC_INFO;
        p->ctrl = LDC_VERS;
        p->seqid = lp->snd_nxt++;
        
        lp->tx_head = tx_advance(lp, lp->tx_head);
        
        printf("Queued control packet %d\n", i);
        usleep(1000);
    }

    // Simulate sending data packets
    for (i = 0; i < 5; i++) {
        p = &lp->tx_base[lp->tx_head];
        memset(p, 0, sizeof(*p));
        
        p->type = LDC_DATA;
        p->seqid = lp->snd_nxt++;
        snprintf((char *)p->u.u_data, sizeof(p->u.u_data),
                "Test data packet %d", i);
        
        lp->tx_head = tx_advance(lp, lp->tx_head);
        
        printf("Queued data packet %d\n", i);
        usleep(1000);
    }

    pthread_spin_unlock(&lp->lock);
}

int main(void)
{
    struct ldc_channel_config cfg = {
        .mode = 0,
        .debug = LDC_DEBUG_ALL
    };
    struct ldc_channel *lp;
    int err;

    printf("LDC (Logical Domain Channel) Test Program\n");
    printf("========================================\n\n");

    // Create and initialize channel
    lp = ldc_alloc(1, &cfg);
    if (!lp) {
        printf("Failed to allocate LDC channel\n");
        return 1;
    }
    printf("Created LDC channel %lu\n", lp->id);

    // Bind the channel
    err = ldc_bind(lp);
    if (err) {
        printf("Failed to bind LDC channel\n");
        ldc_free(lp);
        return 1;
    }
    printf("Bound LDC channel %lu\n", lp->id);

    // Start the RX and TX threads
    lp->is_running = true;
    pthread_create(&lp->rx_thread, NULL, rx_thread, lp);
    pthread_create(&lp->tx_thread, NULL, tx_thread, lp);

    // Run test scenarios
    printf("\nStarting packet exchange simulation...\n\n");
    simulate_packet_exchange(lp);

    // Let the simulation run for a while
    sleep(2);

    // Cleanup
    lp->is_running = false;
    pthread_join(lp->rx_thread, NULL);
    pthread_join(lp->tx_thread, NULL);
    
    printf("\nCleaning up...\n");
    ldc_free(lp);
    printf("Test completed successfully\n");

    return 0;
}
