#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib.h> // For g_new0

#ifndef CPU_STATS_H
#define CPU_STATS_H

#define MAX_DEBUG_FUNCS 128  /* Maximum number of functions we'll track */
#define NUM_STATES 3 /* PROM_IDLE, OS_IDLE, SHUTDOWN */
#define STATE_EDGE_FILE "state_edges.dat"

/* Structure to hold the min/max edges for counters per function */
struct func_state_edges {
    uint64_t min_ns[2][2]; // Min/Max observed value for min_ns[0/1]
    uint64_t max_ns[2][2]; // Min/Max observed value for max_ns[0/1]
    uint64_t avg_ns[2][2]; // Min/Max observed value for avg_ns[0/1]
    int min_streak[2][2]; // Min/Max observed value for min_streak[0/1]
    int max_streak[2][2]; // Min/Max observed value for max_streak[0/1]
    int true_count[2][2]; // Min/Max observed value for true_count[0/1]
    int false_count[2][2]; // Min/Max observed value for false_count[0/1]
};

/* Structure to hold edges for all functions within a specific state */
struct state_edges {
    struct func_state_edges func_edges[MAX_DEBUG_FUNCS];
};

/* Global variables for debug counters */
struct debug_counters *counters_array = NULL;
int next_func_id = 0;

/* Global variable for state edges */
struct state_edges state_edge_data[NUM_STATES];
bool state_edges_loaded = false;

void cpu_stats_per_second(void);

/* Debug function macro to collect statistics */
#define DEBUG_FUNC() \
    static struct debug_counters *counters = NULL; \
    do { \
        if (!counters) { \
            int func_id = next_func_id++; \
            if (func_id <= MAX_DEBUG_FUNCS) { \
                if (!counters_array) \
                    counters_array = g_new0(struct debug_counters, MAX_DEBUG_FUNCS); \
                counters = &counters_array[func_id - 1]; \
                counters->func_name = __func__; \
            } \
        } \
        if (counters) counters->call_count++; \
        cpu_stats_per_second(); \
    } while (0)

/* Helper macros to track return values with timing */
#define DEBUG_RETURN(x) do { \
    if (counters) { \
        struct timespec current_ts; \
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &current_ts); \
        if (counters->last_time[x].tv_sec != 0 || counters->last_time[x].tv_nsec != 0) { \
            uint64_t interval = (current_ts.tv_sec - counters->last_time[x].tv_sec) * 1000000000ULL + \
                               (current_ts.tv_nsec - counters->last_time[x].tv_nsec); \
            counters->total_ns[x] += interval; \
            if (interval < counters->min_ns[x] || counters->min_ns[x] == 0) \
                counters->min_ns[x] = interval; \
            if (interval > counters->max_ns[x]) \
                counters->max_ns[x] = interval; \
        } \
        counters->last_time[x] = current_ts; \
        counters->count[x]++; \
        counters->current_streak[x]++; \
        if (counters->current_streak[1-x] && \
            (!counters->min_streak[1-x] || counters->current_streak[1-x] < counters->min_streak[1-x])) \
            counters->min_streak[1-x] = counters->current_streak[1-x]; \
        counters->current_streak[1-x] = 0; \
        if (counters->current_streak[x] > counters->max_streak[x]) \
            counters->max_streak[x] = counters->current_streak[x]; \
    } \
    return true; \
} while (0)

#endif /* CPU_STATS_H */
