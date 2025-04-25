#ifndef CPU_STATS_H
#define CPU_STATS_H

void cpu_stats_print_all(void);

/* Structure to hold debug counters for each function */
struct debug_counters {
    int call_count;
    int true_count;
    int false_count;
    const char *func_name;
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

/* Helper macros to track return values */
#define DEBUG_RETURN_TRUE() do { \
    if (counters) counters->true_count++; \
    return true; \
} while (0)

#define DEBUG_RETURN_FALSE() do { \
    if (counters) counters->false_count++; \
    return false; \
} while (0)

#endif /* CPU_STATS_H */
