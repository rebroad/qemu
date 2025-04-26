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

void load_state_edge_data() {
	// TODO
}

void save_state_edge_data() {
	// TODO
}

void update_state_edges(int vm_state) {
	// TODO we need to update the min and max of each of the counters. This data also needs to be loaded from disk (if empty) and saved to disk (when not loading!).
    // TODO this function need to look at all the current counters for all the functions and create/update a template (for all those functions) of max and min values for each of the counters.
	// The format of the saved file needs to include headings for each state (0, 1 and 2), and subheadings for each func_name, then the values of each variable within that function.
	// "each variable" refers to: min_streak, max_streak, avg_ns, min_ns, max_ns
	// so we need to record the edges of each of those variables (i.e. the edges of the window that they operate within - i.e. the minimum and maximum that the values reached during training per the applicable "state", i.e. idle_prom, idle_os, shutdown).
}

bool is_within_state_edges(int vm_state) {
	// TODO this function will simply return true or false confirming if our current stats (for all functions) git within the edges for the state specified.
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
