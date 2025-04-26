#ifndef CPU_STATS_H
#define CPU_STATS_H

#define MAX_DEBUG_FUNCS 128  /* Maximum number of functions we'll track */

/* Debug counter structure for each function */
struct debug_counters {
    const char *func_name;
    int func_id;           /* Unique ID for this function */
    int call_count;
    int true_count;
    int false_count;

    /* Streak tracking */
    int current_true_streak;
    int current_false_streak;
    int min_true_streak;
    int max_true_streak;
    int min_false_streak;
    int max_false_streak;

    /* Timing stats */
    uint64_t min_true_ns;
    uint64_t max_true_ns;
    uint64_t min_false_ns;
    uint64_t max_false_ns;
    struct timespec last_true_time;
    struct timespec last_false_time;
};

/* Global variables */
extern struct debug_counters *counters_array; // REBTODO - should be an array?
extern int next_func_id;

/* Function to print debug statistics */
void cpu_stats_print_all(void);

/* Debug function macro to collect statistics */
#define DEBUG_FUNC() \
    static int func_id = 0; \
    static struct debug_counters *counters = NULL; \
    do { \
        if (func_id == 0) { \
            func_id = next_func_id++; \
            if (func_id > MAX_FUNCS) { \
                fprintf(stderr, "Warning: Exceeded maximum number of tracked functions (%d)\n", MAX_FUNCS); \
                func_id = -1; \
            } else { \
                if (counters_array == NULL) \
                    counters_array = g_new0(struct debug_counters, MAX_FUNCS); \
                counters = &counters_array[func_id - 1]; \
                counters->func_name = __func__; \
            } \
        } \
        if (counters) counters->call_count++; \
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
