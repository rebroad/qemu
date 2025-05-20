#ifndef CPU_STATS_H
#define CPU_STATS_H

#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#define MAX_DEBUG_FUNCS 128  /* Maximum number of functions we'll track */

struct debug_counters {
    const char *func_name;
    const char *file_name;
    int call_count;
    int count[2];  // [false, true]
    int current_streak[2];
    int min_streak[2];
    int max_streak[2];
    uint64_t total_ns[2];
    uint64_t min_ns[2];
    uint64_t max_ns[2];
    struct timespec last_time[2];  // Last time we got true/false result
};

// Global variables for debug counters
extern struct debug_counters *counters_array;  // Memory allocated on first use

// Function to find or create counters for a function
struct debug_counters *find_counters_array(const char *func_name, const char *file_name);

// Debug macros for function instrumentation
#define DEBUG_FUNC() \
    static struct debug_counters *counters = NULL; \
    do { \
        if (!counters) counters = find_counters_array(__func__, __FILE__); \
        if (counters) counters->call_count++; \
    } while (0)

#define DEBUG_RETURN(n) do { \
    if (counters) { \
        struct timespec now; \
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now); \
        int x = (n) ? 1 : 0, y = (n) ? 0 : 1; \
        if (counters->last_time[x].tv_sec != 0 || counters->last_time[x].tv_nsec != 0) { \
            uint64_t diff_ns = (now.tv_sec - counters->last_time[x].tv_sec) * 1000000000ULL + \
                             (now.tv_nsec - counters->last_time[x].tv_nsec); \
            counters->total_ns[x] += diff_ns; \
            if (diff_ns < counters->min_ns[x] || !counters->min_ns[x]) \
                counters->min_ns[x] = diff_ns; \
            if (diff_ns > counters->max_ns[x]) counters->max_ns[x] = diff_ns; \
        } \
        counters->last_time[x] = now; \
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
