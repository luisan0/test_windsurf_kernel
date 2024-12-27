#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>

/* Define the unlikely macro */
#define unlikely(x) __builtin_expect(!!(x), 0)

typedef uint32_t u32;

/* A single data point for our parameterized min-max tracker */
struct minmax_sample {
    u32 t;      /* time measurement was taken */
    u32 v;      /* value measured */
};

/* State for the parameterized min-max tracker */
struct minmax {
    struct minmax_sample s[3];
};

/* Get the current tracked value */
static inline u32 minmax_get(const struct minmax *m)
{
    return m->s[0].v;
}

/* Reset the tracker with a new measurement */
static inline u32 minmax_reset(struct minmax *m, u32 t, u32 meas)
{
    struct minmax_sample val = { .t = t, .v = meas };
    m->s[2] = m->s[1] = m->s[0] = val;
    return m->s[0].v;
}

/* As time advances, update the 1st, 2nd, and 3rd choices */
static u32 minmax_subwin_update(struct minmax *m, u32 win,
                               const struct minmax_sample *val)
{
    u32 dt = val->t - m->s[0].t;

    if (unlikely(dt > win)) {
        /*
         * Passed entire window without a new val so make 2nd
         * choice the new val & 3rd choice the new 2nd choice.
         */
        m->s[0] = m->s[1];
        m->s[1] = m->s[2];
        m->s[2] = *val;
        if (unlikely(val->t - m->s[0].t > win)) {
            m->s[0] = m->s[1];
            m->s[1] = m->s[2];
            m->s[2] = *val;
        }
    } else if (unlikely(m->s[1].t == m->s[0].t) && dt > win/4) {
        /*
         * We've passed a quarter of the window without a new val
         * so take a 2nd choice from the 2nd quarter of the window.
         */
        m->s[2] = m->s[1] = *val;
    } else if (unlikely(m->s[2].t == m->s[1].t) && dt > win/2) {
        /*
         * We've passed half the window without finding a new val
         * so take a 3rd choice from the last half of the window
         */
        m->s[2] = *val;
    }
    return m->s[0].v;
}

/* Check if new measurement updates the 1st, 2nd or 3rd choice max */
u32 minmax_running_max(struct minmax *m, u32 win, u32 t, u32 meas)
{
    struct minmax_sample val = { .t = t, .v = meas };

    if (unlikely(val.v >= m->s[0].v) ||        /* found new max? */
        unlikely(val.t - m->s[2].t > win))     /* nothing left in window? */
        return minmax_reset(m, t, meas);       /* forget earlier samples */

    if (unlikely(val.v >= m->s[1].v))
        m->s[2] = m->s[1] = val;
    else if (unlikely(val.v >= m->s[2].v))
        m->s[2] = val;

    return minmax_subwin_update(m, win, &val);
}

/* Check if new measurement updates the 1st, 2nd or 3rd choice min */
u32 minmax_running_min(struct minmax *m, u32 win, u32 t, u32 meas)
{
    struct minmax_sample val = { .t = t, .v = meas };

    if (unlikely(val.v <= m->s[0].v) ||        /* found new min? */
        unlikely(val.t - m->s[2].t > win))     /* nothing left in window? */
        return minmax_reset(m, t, meas);       /* forget earlier samples */

    if (unlikely(val.v <= m->s[1].v))
        m->s[2] = m->s[1] = val;
    else if (unlikely(val.v <= m->s[2].v))
        m->s[2] = val;

    return minmax_subwin_update(m, win, &val);
}

/* Simulate network RTT measurements */
u32 simulate_rtt(u32 base_rtt)
{
    /* Add some random variation to the base RTT */
    int variation = rand() % 20 - 10;  /* -10 to +10 ms variation */
    return (u32)((int)base_rtt + variation);
}

/* Helper function to print minmax state */
void print_minmax_state(const struct minmax *m, const char *prefix)
{
    printf("%s: [%u@%u] [%u@%u] [%u@%u]\n", prefix,
           m->s[0].v, m->s[0].t,
           m->s[1].v, m->s[1].t,
           m->s[2].v, m->s[2].t);
}

int main()
{
    struct minmax min_tracker, max_tracker;
    u32 base_rtt = 50;  /* Base RTT of 50ms */
    u32 window = 100;   /* 100 time units window */
    u32 current_time = 0;
    
    /* Initialize the trackers */
    minmax_reset(&min_tracker, current_time, base_rtt);
    minmax_reset(&max_tracker, current_time, base_rtt);
    
    printf("Simulating network RTT measurements over time...\n");
    printf("Window size: %u time units\n", window);
    printf("Base RTT: %u ms\n\n", base_rtt);

    /* Simulate measurements over time */
    for (int i = 0; i < 20; i++) {
        current_time += 5;  /* Advance time by 5 units */
        u32 rtt = simulate_rtt(base_rtt);
        
        /* Update min and max trackers */
        u32 min_rtt = minmax_running_min(&min_tracker, window, current_time, rtt);
        u32 max_rtt = minmax_running_max(&max_tracker, window, current_time, rtt);
        
        printf("Time %3u: RTT=%3u ms, Window Min=%3u ms, Window Max=%3u ms\n",
               current_time, rtt, min_rtt, max_rtt);
        
        /* Print detailed state every 5 measurements */
        if (i % 5 == 4) {
            printf("\nDetailed state:\n");
            print_minmax_state(&min_tracker, "Min tracker");
            print_minmax_state(&max_tracker, "Max tracker");
            printf("\n");
        }
    }

    return 0;
}
