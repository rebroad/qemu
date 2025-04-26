#ifndef CPU_STATS_H
#define CPU_STATS_H

#define MAX_DEBUG_FUNCS 128  /* Maximum number of functions we'll track */

/* System state flags */
#define STATE_AUTODETECT  -1
#define STATE_PROM_IDLE   0
#define STATE_OS_IDLE     1
#define STATE_SHUTDOWN    2
#define NUM_STATES        3  // PROM_IDLE, OS_IDLE, SHUTDOWN

static struct state_edges[NUM_STATES];

/* Debug counter structure for each function */
struct debug_counters {
    const char *func_name;
    int call_count;
    int count[2]; // True & false

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

/* Global variables */
extern struct debug_counters *counters_array; // Memory allocated on first use
extern int next_func_id;

/* Function to print debug statistics */
void cpu_stats_print_all(void) {
    if (next_func_id > MAX_FUNCS)
        fprintf(stderr, "Warning: Exceeded maximum number of tracked functions (%d)\n", MAX_FUNCS);

    printf("\nDebug Statistics Summary:\n");
    printf("======================\n");
    // TODO report is_on_battery, idle_prom, idle_os, shutdown_indicated

    for (int i = 0; i < next_func_id; i++) {
        struct debug_counters *it = &counters_array[i];
        if (it->count[0] == 0 && it->count[1] == 0)
            printf("%s: called %d times\n", it->func_name, it->call_count);
        else {
            uint64_t avg_false_ns = it->count[0] ? it->total_ns[0] / it->count[0] : -1;
            uint64_t avg_true_ns = it->count[1] ? it->total_ns[1] / it->count[1] : -1;
            printf("%s: true=%d (avg/min/max=%" PRIu64 "/%" PRIu64 "/%" PRIu64 " ns, streak=%d-%d), "
                   "false=%d (avg/min/max=%" PRIu64 "/%" PRIu64 "/%" PRIu64 " ns, streak=%d-%d)\n",
                   it->func_name,
                   it->count[1], avg_true_ns, it->min_ns[1], it->max_ns[1],
                   it->min_streak[1], it->max_streak[1],
                   it->count[0], avg_false_ns, it->min_ns[0], it->max_ns[0],
                   it->min_streak[0], it->max_streak[0]);
        }
    }
}

static int get_system_state(current_state) {
    int new_state = STATE_AUTODETECT;
    FILE *state_file = fopen("vm_state.txt", "r");
    if (state_file) {
        if (fscanf(state_file, "%d", &new_state) == 1 && (new_state != current_state))
            qemu_log("State change detected: %d -> %d\n", current_state, new_state);
        fclose(state_file);
    }

	return new_state;
}

void reset_cpu_stats(void) {
    for (int i = 0; i < next_func_id; i++) {
        struct debug_counters *it = &counters_array[i];
        it->call_count = 0;
        for (int j = 0; i < 2; j++) // True & false
            it->count[j] = it->min_streak[j] = it->max_streak[j] = it->total_ns[j] = it->min_ns[j] = it->max_ns[j] = 0;
    }
}

void update_state_edges(int vm_state) {
	// TODO we need to update the min and max of each of the counters. This data also needs to be loaded from disk (if empty) and saved to disk (when not loading!).
    // TODO this function need to look at all the current counters for all the functions and create/update a template (for all those functions) of max and min values for each of the counters.
	// The format of the saved file needs to include headings for each state (0, 1 and 2), and subheadings for each func_name, then the values of each variable within that function.
	// "each variable" refers to: min_streak, max_streak, avg_ns, min_ns, max_ns
	// so we need to record the edges of each of those variables (i.e. the edges of the window that they operate within - i.e. the minimum and maximum that the values reached during training per the applicable "state", i.e. idle_prom, idle_os, shutdown).
}

void cpu_stats_per_second(void) {
    static int vm_state = STATE_AUTODETECT;

    time_t current_time = time(NULL);
    static time_t last_stats_print = current_time;
    if (current_time = last_stats_print) return;
    last_stats_print = current_time;

    cpu_stats_print_all();
    vm_state = get_system_state(vm_state);
	update_state_edges(vm_state);
    reset_cpu_stats();
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
