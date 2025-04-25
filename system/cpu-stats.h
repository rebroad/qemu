#ifndef CPU_STATS_H
#define CPU_STATS_H

void cpu_stats_print_all(void);

/* Structure to hold debug counters for each function */
struct debug_counters {
    const char *func_name;
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
    struct timespec last_true_time;
    struct timespec last_false_time;
    uint64_t total_true_ns;
    uint64_t total_false_ns;
    uint64_t min_true_ns;
    uint64_t max_true_ns;
    uint64_t min_false_ns;
    uint64_t max_false_ns;
};

/* Array to store counters, with a reasonable initial size */
#define MAX_FUNCS 128
extern struct debug_counters *counters_array;
extern int next_func_id;

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
#define DEBUG_RETURN_TRUE() do { \
    if (counters) { \
        struct timespec current_ts; \
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &current_ts); \
        if (counters->last_true_time.tv_sec != 0 || counters->last_true_time.tv_nsec != 0) { \
            uint64_t interval = (current_ts.tv_sec - counters->last_true_time.tv_sec) * 1000000000ULL + \
                               (current_ts.tv_nsec - counters->last_true_time.tv_nsec); \
            counters->total_true_ns += interval; \
            if (interval < counters->min_true_ns || counters->min_true_ns == 0) \
                counters->min_true_ns = interval; \
            if (interval > counters->max_true_ns) \
                counters->max_true_ns = interval; \
        } \
        counters->last_true_time = current_ts; \
        counters->true_count++; \
        counters->current_true_streak++; \
        if (counters->current_false_streak && \
            (!counters->min_false_streak || counters->current_false_streak < counters->min_false_streak)) \
            counters->min_false_streak = counters->current_false_streak; \
        counters->current_false_streak = 0; \
        if (counters->current_true_streak > counters->max_true_streak) \
            counters->max_true_streak = counters->current_true_streak; \
    } \
    return true; \
} while (0)

#define DEBUG_RETURN_FALSE() do { \
    if (counters) { \
        struct timespec current_ts; \
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &current_ts); \
        if (counters->last_false_time.tv_sec != 0 || counters->last_false_time.tv_nsec != 0) { \
            uint64_t interval = (current_ts.tv_sec - counters->last_false_time.tv_sec) * 1000000000ULL + \
                               (current_ts.tv_nsec - counters->last_false_time.tv_nsec); \
            counters->total_false_ns += interval; \
            if (interval < counters->min_false_ns || counters->min_false_ns == 0) \
                counters->min_false_ns = interval; \
            if (interval > counters->max_false_ns) \
                counters->max_false_ns = interval; \
        } \
        counters->last_false_time = current_ts; \
        counters->false_count++; \
        counters->current_false_streak++; \
        if (counters->current_true_streak && \
            (!counters->min_true_streak || counters->current_true_streak < counters->min_true_streak)) \
            counters->min_true_streak = counters->current_true_streak; \
        counters->current_true_streak = 0; \
        if (counters->current_false_streak > counters->max_false_streak) \
            counters->max_false_streak = counters->current_false_streak; \
    } \
    return false; \
} while (0)

#endif /* CPU_STATS_H */
