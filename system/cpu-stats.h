#ifndef CPU_STATS_H
#define CPU_STATS_H

#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib.h> // For g_new0, g_error
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
struct debug_counters *counters_array = NULL; // Memory allocated on first use

/* Function declarations */
void cpu_stats_per_second(void);
struct debug_counters *find_counters_array(const char *func_name);

/* Prototype for the internal TLS helper function (defined in cpu-stats.c) */
struct debug_counters *_cpu_stats_get_tls_counters(const char *current_func_name);

/* --- Macros --- */

/* Debug function macro - calls helper */
#define DEBUG_FUNC() \
    do { \
        struct debug_counters *_counters = _cpu_stats_get_tls_counters(__func__); \
        if (_counters) _counters->call_count++; \
    } while (0)

/* Helper macro for return values - calls helper */
#define DEBUG_RETURN(n) \
    do { \
        struct debug_counters *_counters = _cpu_stats_get_tls_counters(__func__); \
        if (_counters) { \
            struct timespec current_ts; \
            clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &current_ts); \
            int x = (n) ? 1 : 0, y = (n) ? 0 : 1; \
            if (_counters->last_time[x].tv_sec != 0 || _counters->last_time[x].tv_nsec != 0) { \
                uint64_t interval = (current_ts.tv_sec - _counters->last_time[x].tv_sec) * 1000000000ULL + \
                                   (current_ts.tv_nsec - _counters->last_time[x].tv_nsec); \
                _counters->total_ns[x] += interval; \
                if (interval < _counters->min_ns[x] || !_counters->min_ns[x]) \
                    _counters->min_ns[x] = interval; \
                if (interval > _counters->max_ns[x]) _counters->max_ns[x] = interval; \
            } \
            _counters->last_time[x] = current_ts; \
            _counters->count[x]++; \
            _counters->current_streak[x]++; \
            if (_counters->current_streak[y] && \
                (!_counters->min_streak[y] || _counters->current_streak[y] < _counters->min_streak[y])) \
                _counters->min_streak[y] = _counters->current_streak[y]; \
            _counters->current_streak[y] = 0; \
            if (_counters->current_streak[x] > _counters->max_streak[x]) \
                _counters->max_streak[x] = _counters->current_streak[x]; \
        } \
        return n; \
    } while (0)

#endif /* CPU_STATS_H */
