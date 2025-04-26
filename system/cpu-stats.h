#ifndef CPU_STATS_H
#define CPU_STATS_H

#define MAX_DEBUG_FUNCS 128  /* Maximum number of functions we'll track */

/* Debug counter structure for each function */
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
void cpu_stats_print_all(void) {
    if (next_func_id > MAX_FUNCS)
        fprintf(stderr, "Warning: Exceeded maximum number of tracked functions (%d)\n", MAX_FUNCS);

    printf("\nDebug Statistics Summary:\n");
    printf("======================\n");

    for (int i = 0; i < next_func_id - 1; i++) {
        struct debug_counters *counters = &counters_array[i];
        if (counters->true[1] == 0 && counters->false[0] == 0)
            printf("%s: called %d times\n", counters->func_name, counters->call_count);
        else {
            uint64_t avg_true_ns = counters->count[1] ? counters->total_true_ns / counters->true_count : 0;
            uint64_t avg_false_ns = counters->count[0] ? counters->total_false_ns / counters->false_count : 0;
            printf("%s: true=%d (avg/min/max=%" PRIu64 "/%" PRIu64 "/%" PRIu64 " ns, streak=%d-%d), "
                   "false=%d (avg/min/max=%" PRIu64 "/%" PRIu64 "/%" PRIu64 " ns, streak=%d-%d)\n",
                   counters->func_name,
                   counters->true_count, avg_true_ns, counters->min_true_ns, counters->max_true_ns,
                   counters->min_true_streak, counters->max_true_streak,
                   counters->false_count, avg_false_ns, counters->min_false_ns, counters->max_false_ns,
                   counters->min_false_streak, counters->max_false_streak);
        }
    }
}

static void check_system_state(void) {
    FILE *state_file = fopen("vm_state.txt", "r");
    if (state_file) {
        int new_state;
        if (fscanf(state_file, "%d", &new_state) == 1) {
            if (new_state != current_state) {
                qemu_log("State change detected: %d -> %d\n", current_state, new_state);
                system_state_update(NULL); // Reset current measurements
            }
            current_state = new_state;
            current_mode = MODE_TRAINING;
        } // TODO - go out of TRAINING mode for invalid states
        fclose(state_file);
    }
    last_state_check = current_time;
}

void cpu_stats_per_second(void) {
	time_t current_time = time(NULL);
	static time_t last_stats_print = current_time;
	if (current_time = last_stats_print) return;
	last_stats_print = current_time;
	cpu_stats_print_all();
	check_system_state();
	system_state_print_stats();
}

/* Debug function macro to collect statistics */
#define DEBUG_FUNC() \
    static struct debug_counters *counters = NULL; \
    do { \
        if (!counters) { \
            int func_id = next_func_id++; \
			if (func_id <= MAX_FUNCS) { \
                if (!counters_array) \
                    counters_array = g_new0(struct debug_counters, MAX_FUNCS); \
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
