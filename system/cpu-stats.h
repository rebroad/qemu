#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib.h> // For g_new0
#include <stdbool.h>
#include <limits.h> // For UINT64_MAX, INT_MAX

#ifndef CPU_STATS_H
#define CPU_STATS_H

#define MAX_DEBUG_FUNCS 128  /* Maximum number of functions we'll track */
#define NUM_STATES 3 /* PROM_IDLE, OS_IDLE, SHUTDOWN */
#define STATE_EDGE_FILE "state_edges.dat"
#define MAX_FUNC_NAME_LEN 64 // Max length for function names
#define STATE_EDGE_MAGIC 0xCPU57A75 // Magic number for file format
#define STATE_EDGE_VERSION 1        // File format version

/* Structure to hold the min/max edges for counters per function */
/* This structure is used both in the save file and runtime */
struct func_state_edges {
    char func_name[MAX_FUNC_NAME_LEN]; // Function name associated with these edges
    // Format: stat[true/false][min/max]
    uint64_t min_ns[2][2];
    uint64_t max_ns[2][2];
    uint64_t avg_ns[2][2];
    int min_streak[2][2];
    int max_streak[2][2];
    int count[2][2]; // Min/Max observed value for count[0/1]
};

/* Structure to hold edges for all functions within a specific state */
struct state_edges {
    // Note: func_name is now part of func_state_edges
    struct func_state_edges func_edges[MAX_DEBUG_FUNCS];
};

/* Header structure for the state edge file */
struct state_edge_file_header {
    uint32_t magic;
    uint32_t version;
    uint32_t num_funcs_saved;
};

/* Global variables for debug counters */
struct debug_counters *counters_array = NULL;
int next_func_id = 0;

/* Global variable for state edges */
struct state_edges state_edge_data[NUM_STATES];
bool state_edges_loaded = false;

void cpu_stats_per_second(void);
struct debug_counters *find_counters_array(const char *func_name);

/* Debug function macro to collect statistics */
#define DEBUG_FUNC() \
    static struct debug_counters *counters = NULL; \
    do { \
        if (!counters) { \
            if (next_func_id++ <= MAX_DEBUG_FUNCS) { \
                if (!counters_array) \
                    counters_array = g_new0(struct debug_counters, MAX_DEBUG_FUNCS); \
                counters = find_counters_array(__func__); \
            } \
        } \
        if (counters) counters->call_count++; \
        cpu_stats_per_second(); \
    } while (0)

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
            if (interval < counters->min_ns[x] || counters->min_ns[x] == 0) \
                counters->min_ns[x] = interval; \
            if (interval > counters->max_ns[x]) \
                counters->max_ns[x] = interval; \
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
