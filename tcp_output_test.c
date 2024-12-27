/*
 * TCP Output Test Program
 * This is a standalone simulation of TCP output operations
 * Author: Cascade AI
 * Date: 2024-12-27
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <errno.h>

/* Constants */
#define TCP_MAX_WINDOW     65535
#define TCP_MSS            1460
#define TCP_MIN_MSS        536
#define TCP_HEADER_SIZE    20
#define IP_HEADER_SIZE     20
#define MAX_SEGMENTS       32
#define MAX_RETRIES        5
#define RTO_MIN           1000    /* 1 second */
#define RTO_MAX           120000  /* 2 minutes */
#define INIT_CWND         10
#define INIT_SSTHRESH     65535

/* TCP Flags */
#define TCP_FIN           0x01
#define TCP_SYN           0x02
#define TCP_RST           0x04
#define TCP_PSH           0x08
#define TCP_ACK           0x10
#define TCP_URG           0x20
#define TCP_ECE           0x40
#define TCP_CWR           0x80

/* TCP States */
#define TCP_ESTABLISHED   1
#define TCP_SYN_SENT     2
#define TCP_SYN_RECV     3
#define TCP_FIN_WAIT1    4
#define TCP_FIN_WAIT2    5
#define TCP_TIME_WAIT    6
#define TCP_CLOSE        7
#define TCP_CLOSE_WAIT   8
#define TCP_LAST_ACK     9
#define TCP_LISTEN       10
#define TCP_CLOSING      11

/* TCP Options */
#define TCP_OPT_EOL      0
#define TCP_OPT_NOP      1
#define TCP_OPT_MSS      2
#define TCP_OPT_WSCALE   3
#define TCP_OPT_SACK     4
#define TCP_OPT_TSVAL    8

/* TCP Segment */
struct tcp_segment {
    uint32_t seq;
    uint32_t ack;
    uint16_t window;
    uint8_t  flags;
    uint16_t mss;
    uint8_t  data[TCP_MSS];
    uint16_t len;
    uint32_t tsval;
    uint32_t tsecr;
    bool     sacked;
    bool     retrans;
    uint8_t  retries;
};

/* TCP Socket */
struct tcp_sock {
    /* Connection info */
    uint32_t saddr;
    uint32_t daddr;
    uint16_t sport;
    uint16_t dport;
    uint8_t  state;
    
    /* Sequence numbers */
    uint32_t snd_una;    /* First unacknowledged byte */
    uint32_t snd_nxt;    /* Next sequence to send */
    uint32_t rcv_nxt;    /* Next sequence expected */
    uint32_t rcv_wnd;    /* Receive window */
    
    /* Congestion control */
    uint32_t cwnd;       /* Congestion window */
    uint32_t ssthresh;   /* Slow start threshold */
    bool     in_recovery;
    uint32_t recover;    /* Recovery point */
    
    /* RTT estimation */
    uint32_t srtt;       /* Smoothed RTT */
    uint32_t rttvar;     /* RTT variation */
    uint32_t rto;        /* Retransmission timeout */
    
    /* Send buffer */
    struct tcp_segment segments[MAX_SEGMENTS];
    uint8_t  num_segments;
    
    /* Statistics */
    uint64_t packets_sent;
    uint64_t bytes_sent;
    uint64_t retransmits;
    uint64_t timeouts;
};

/* Function declarations */
static struct tcp_sock *tcp_sock_create(void);
static void tcp_sock_destroy(struct tcp_sock *sk);
static int tcp_transmit_skb(struct tcp_sock *sk, struct tcp_segment *seg);
static void tcp_retransmit_skb(struct tcp_sock *sk, struct tcp_segment *seg);
static void tcp_write_xmit(struct tcp_sock *sk);
static void tcp_retransmit_timer(struct tcp_sock *sk);
static void tcp_cwnd_event(struct tcp_sock *sk, int event);
static void tcp_ack_received(struct tcp_sock *sk, uint32_t ack, uint16_t window);
static void tcp_clean_rtx_queue(struct tcp_sock *sk, uint32_t ack);
static void tcp_update_rtt(struct tcp_sock *sk, uint32_t rtt);
static void tcp_enter_recovery(struct tcp_sock *sk);
static void tcp_leave_recovery(struct tcp_sock *sk);
static void print_segment(const struct tcp_segment *seg);
static void print_sock_stats(const struct tcp_sock *sk);
static const char *tcp_state_str(uint8_t state);

/* Create TCP socket */
static struct tcp_sock *tcp_sock_create(void) {
    struct tcp_sock *sk = calloc(1, sizeof(*sk));
    if (!sk)
        return NULL;
    
    /* Initialize socket */
    sk->saddr = 0x0A000001;  /* 10.0.0.1 */
    sk->daddr = 0x0A000002;  /* 10.0.0.2 */
    sk->sport = 12345;
    sk->dport = 80;
    sk->state = TCP_ESTABLISHED;
    
    /* Initialize sequence numbers */
    sk->snd_una = 1000;
    sk->snd_nxt = sk->snd_una;
    sk->rcv_nxt = 2000;
    sk->rcv_wnd = TCP_MAX_WINDOW;
    
    /* Initialize congestion control */
    sk->cwnd = INIT_CWND * TCP_MSS;
    sk->ssthresh = INIT_SSTHRESH;
    
    /* Initialize RTT variables */
    sk->srtt = 100;      /* 100ms initial RTT */
    sk->rttvar = 50;     /* 50ms initial variation */
    sk->rto = RTO_MIN;
    
    return sk;
}

/* Destroy TCP socket */
static void tcp_sock_destroy(struct tcp_sock *sk) {
    if (sk)
        free(sk);
}

/* Transmit TCP segment */
static int tcp_transmit_skb(struct tcp_sock *sk, struct tcp_segment *seg) {
    printf("Transmitting segment: seq=%u, len=%u, flags=0x%02x\n",
           seg->seq, seg->len, seg->flags);
    
    /* Update statistics */
    sk->packets_sent++;
    sk->bytes_sent += seg->len;
    
    if (seg->retrans)
        sk->retransmits++;
    
    return 0;
}

/* Retransmit TCP segment */
static void tcp_retransmit_skb(struct tcp_sock *sk, struct tcp_segment *seg) {
    if (seg->retries >= MAX_RETRIES) {
        printf("Max retries reached for segment seq=%u\n", seg->seq);
        return;
    }
    
    seg->retrans = true;
    seg->retries++;
    tcp_transmit_skb(sk, seg);
    
    /* Exponential backoff of RTO */
    sk->rto = (sk->rto * 2 < RTO_MAX) ? sk->rto * 2 : RTO_MAX;
}

/* Write data to transmit queue */
static void tcp_write_xmit(struct tcp_sock *sk) {
    uint32_t cwnd_avail = sk->cwnd;
    uint32_t mss = TCP_MSS;
    uint8_t i;
    
    printf("\nWriting data to transmit queue...\n");
    printf("Available cwnd: %u bytes\n", cwnd_avail);
    
    /* Create segments up to cwnd */
    for (i = 0; i < MAX_SEGMENTS && cwnd_avail >= mss; i++) {
        struct tcp_segment *seg = &sk->segments[sk->num_segments];
        
        /* Initialize segment */
        seg->seq = sk->snd_nxt;
        seg->ack = sk->rcv_nxt;
        seg->window = sk->rcv_wnd;
        seg->flags = TCP_ACK;
        seg->mss = mss;
        seg->len = mss;
        seg->retrans = false;
        seg->retries = 0;
        
        /* Generate some test data */
        memset(seg->data, 'A' + i, mss);
        
        /* Update sequence number and window */
        sk->snd_nxt += seg->len;
        cwnd_avail -= seg->len;
        
        /* Transmit segment */
        tcp_transmit_skb(sk, seg);
        sk->num_segments++;
    }
    
    printf("Created %u segments\n", i);
}

/* Handle retransmission timeout */
static void tcp_retransmit_timer(struct tcp_sock *sk) {
    uint8_t i;
    
    printf("\nRetransmission timer expired...\n");
    
    /* Enter recovery if not already */
    if (!sk->in_recovery)
        tcp_enter_recovery(sk);
    
    /* Retransmit unacked segments */
    for (i = 0; i < sk->num_segments; i++) {
        struct tcp_segment *seg = &sk->segments[i];
        if (seg->seq < sk->snd_una)
            tcp_retransmit_skb(sk, seg);
    }
    
    sk->timeouts++;
}

/* Handle congestion events */
static void tcp_cwnd_event(struct tcp_sock *sk, int event) {
    switch (event) {
    case 0:  /* Duplicate ACK */
        if (!sk->in_recovery) {
            sk->ssthresh = sk->cwnd / 2;
            sk->cwnd = sk->ssthresh + 3 * TCP_MSS;
            tcp_enter_recovery(sk);
        }
        break;
        
    case 1:  /* Timeout */
        sk->ssthresh = sk->cwnd / 2;
        sk->cwnd = TCP_MSS;
        break;
        
    case 2:  /* New ACK */
        if (sk->cwnd < sk->ssthresh)
            sk->cwnd += TCP_MSS;  /* Slow start */
        else
            sk->cwnd += TCP_MSS * TCP_MSS / sk->cwnd;  /* Congestion avoidance */
        break;
    }
    
    printf("Cwnd updated: %u bytes (ssthresh=%u)\n",
           sk->cwnd, sk->ssthresh);
}

/* Handle incoming ACK */
static void tcp_ack_received(struct tcp_sock *sk, uint32_t ack, uint16_t window) {
    printf("\nReceived ACK=%u, window=%u\n", ack, window);
    
    if (ack > sk->snd_una) {
        /* Update window */
        sk->rcv_wnd = window;
        
        /* Update RTT if not a retransmission */
        if (!sk->in_recovery)
            tcp_update_rtt(sk, 100);  /* Simulate 100ms RTT */
        
        /* Clean retransmission queue */
        tcp_clean_rtx_queue(sk, ack);
        
        /* Update congestion window */
        tcp_cwnd_event(sk, 2);
        
        /* Leave recovery if all data acked */
        if (sk->in_recovery && ack >= sk->recover)
            tcp_leave_recovery(sk);
        
        sk->snd_una = ack;
    }
}

/* Clean retransmission queue */
static void tcp_clean_rtx_queue(struct tcp_sock *sk, uint32_t ack) {
    uint8_t i = 0;
    
    while (i < sk->num_segments) {
        struct tcp_segment *seg = &sk->segments[i];
        
        if (seg->seq + seg->len <= ack) {
            /* Remove segment */
            if (i < sk->num_segments - 1)
                memmove(&sk->segments[i], &sk->segments[i + 1],
                       (sk->num_segments - i - 1) * sizeof(*seg));
            sk->num_segments--;
            continue;
        }
        i++;
    }
    
    printf("Cleaned RTX queue: %u segments remaining\n", sk->num_segments);
}

/* Update RTT estimation */
static void tcp_update_rtt(struct tcp_sock *sk, uint32_t rtt) {
    int32_t err = rtt - sk->srtt;
    
    sk->srtt += err >> 3;      /* srtt = 7/8 srtt + 1/8 rtt */
    sk->rttvar += (abs(err) - sk->rttvar) >> 2;  /* rttvar = 3/4 rttvar + 1/4 |err| */
    
    /* Update RTO */
    sk->rto = sk->srtt + (sk->rttvar << 2);
    if (sk->rto < RTO_MIN)
        sk->rto = RTO_MIN;
    if (sk->rto > RTO_MAX)
        sk->rto = RTO_MAX;
    
    printf("RTT updated: srtt=%ums, rttvar=%ums, rto=%ums\n",
           sk->srtt, sk->rttvar, sk->rto);
}

/* Enter recovery mode */
static void tcp_enter_recovery(struct tcp_sock *sk) {
    sk->in_recovery = true;
    sk->recover = sk->snd_nxt;
    printf("Entering recovery mode (recover=%u)\n", sk->recover);
}

/* Leave recovery mode */
static void tcp_leave_recovery(struct tcp_sock *sk) {
    sk->in_recovery = false;
    printf("Leaving recovery mode\n");
}

/* Print segment info */
static void print_segment(const struct tcp_segment *seg) {
    printf("  SEQ=%u, ACK=%u, LEN=%u, FLAGS=0x%02x%s%s\n",
           seg->seq, seg->ack, seg->len, seg->flags,
           seg->retrans ? " [RETRANS]" : "",
           seg->sacked ? " [SACKED]" : "");
}

/* Print socket statistics */
static void print_sock_stats(const struct tcp_sock *sk) {
    printf("\nTCP Socket Statistics:\n");
    printf("=====================\n");
    printf("State: %s\n", tcp_state_str(sk->state));
    printf("Packets sent: %lu\n", sk->packets_sent);
    printf("Bytes sent: %lu\n", sk->bytes_sent);
    printf("Retransmissions: %lu\n", sk->retransmits);
    printf("Timeouts: %lu\n", sk->timeouts);
    printf("Current window: %u\n", sk->cwnd);
    printf("Slow start threshold: %u\n", sk->ssthresh);
    printf("RTT: %ums (var=%ums)\n", sk->srtt, sk->rttvar);
    printf("RTO: %ums\n", sk->rto);
}

/* Get TCP state string */
static const char *tcp_state_str(uint8_t state) {
    switch (state) {
    case TCP_ESTABLISHED: return "ESTABLISHED";
    case TCP_SYN_SENT:   return "SYN_SENT";
    case TCP_SYN_RECV:   return "SYN_RECV";
    case TCP_FIN_WAIT1:  return "FIN_WAIT1";
    case TCP_FIN_WAIT2:  return "FIN_WAIT2";
    case TCP_TIME_WAIT:  return "TIME_WAIT";
    case TCP_CLOSE:      return "CLOSE";
    case TCP_CLOSE_WAIT: return "CLOSE_WAIT";
    case TCP_LAST_ACK:   return "LAST_ACK";
    case TCP_LISTEN:     return "LISTEN";
    case TCP_CLOSING:    return "CLOSING";
    default:             return "UNKNOWN";
    }
}

/* Test functions */
static void test_normal_transmission(struct tcp_sock *sk) {
    printf("\nTesting normal transmission...\n");
    printf("============================\n");
    
    /* Write some data */
    tcp_write_xmit(sk);
    
    /* Simulate receiving ACKs */
    tcp_ack_received(sk, sk->snd_una + 5000, TCP_MAX_WINDOW);
    tcp_ack_received(sk, sk->snd_una + 10000, TCP_MAX_WINDOW);
}

static void test_retransmission(struct tcp_sock *sk) {
    printf("\nTesting retransmission...\n");
    printf("=========================\n");
    
    /* Write data */
    tcp_write_xmit(sk);
    
    /* Simulate timeout */
    tcp_retransmit_timer(sk);
    
    /* Receive ACK after retransmission */
    tcp_ack_received(sk, sk->snd_una + 15000, TCP_MAX_WINDOW);
}

int main(void) {
    printf("TCP Output Test Program\n");
    printf("======================\n");
    
    /* Create TCP socket */
    struct tcp_sock *sk = tcp_sock_create();
    if (!sk) {
        printf("Failed to create TCP socket\n");
        return 1;
    }
    
    /* Run tests */
    test_normal_transmission(sk);
    test_retransmission(sk);
    
    /* Print final statistics */
    print_sock_stats(sk);
    
    /* Cleanup */
    tcp_sock_destroy(sk);
    
    printf("\nTest completed successfully!\n");
    return 0;
}
