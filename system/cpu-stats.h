#ifndef CPU_STATS_H
#define CPU_STATS_H

#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib.h> // For g_new0
#include <stdbool.h>

#define MAX_DEBUG_FUNCS 128  /* Maximum number of functions we'll track */

/* Debug counter structure for each function */
struct debug_counters {
    const char *func_name;
    int call_count;
    int count[2]; // False=0, True=1

    /* Streak tracking */
    int current_streak[2];
    int min_streak[2];
    int max_streak[2];

    /* Timing stats */
    uint64_t total_ns[2];
    uint64_t min_ns[2];
    uint64_t max_ns[2];
    struct timespec last_time[2];
};

/* Global variables for debug counters */
extern struct debug_counters *counters_array; // Memory allocated on first use

void cpu_stats_per_second(void);
struct debug_counters *find_counters_array(const char *func_name);

/* Debug function macro to collect statistics */
#define DEBUG_FUNC() \
    static struct debug_counters *counters = NULL; \
    do { \
        if (!counters) counters = find_counters_array(__func__); \
        if (counters) counters->call_count++; \
    } while (0)

// TODO - cpu_stats_per_second() probably better called from a timer?

/* Helper macros to track return values with timing */
#define DEBUG_RETURN(n) do { \
    if (counters) { \
        struct timespec current_ts; \
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &current_ts); \
        int x = (n) ? 1 : 0, y = (n) ? 0 : 1; \
        if (counters->last_time[x].tv_sec != 0 || counters->last_time[x].tv_nsec != 0) { \
            uint64_t interval = (current_ts.tv_sec - counters->last_time[x].tv_sec) * 1000000000ULL + \
                               (current_ts.tv_nsec - counters->last_time[x].tv_nsec); \
            counters->total_ns[x] += interval; \
            if (interval < counters->min_ns[x] || !counters->min_ns[x]) \
                counters->min_ns[x] = interval; \
            if (interval > counters->max_ns[x]) counters->max_ns[x] = interval; \
        } \
        counters->last_time[x] = current_ts; \
        counters->count[x]++; \
        counters->current_streak[x]++; \
        if (counters->current_streak[y] && \
            (!counters->min_streak[y] || counters->current_streak[y] < counters->min_streak[y])) \
            counters->min_streak[y] = counters->current_streak[y]; \
        counters->current_streak[y] = 0; \
        if (counters->current_streak[x] > counters->max_streak[x]) \
            counters->max_streak[x] = counters->current_streak[x]; \
    } \
    return n; \
} while (0)

#endif /* CPU_STATS_H */
