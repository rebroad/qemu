#ifndef CPU_STATS_H
#define CPU_STATS_H

#defome MAX_DEBUG_FUNCS 128  /* Maximum number of functions we'll track */

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
        // TODO - time interval calculation
    } \
    return true; \
} while (0)

#endif /* CPU_STATS_H */
