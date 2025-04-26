#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <stdbool.h>
#include <glib.h> // Include necessary headers

#include "cpu-stats.h" // Include the header file

/* System state flags */
#define STATE_AUTODETECT  -1
#define STATE_PROM_IDLE   0
#define STATE_OS_IDLE     1
#define STATE_SHUTDOWN    2
#define NUM_STATES        3  // PROM_IDLE, OS_IDLE, SHUTDOWN

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
extern struct state_edges state_edge_data[NUM_STATES]; // Defined in header
extern bool state_edges_loaded; // Defined in header

// Placeholders for system state variables - TODO: Might not need these
bool idle_prom = false;
bool idle_os = false;
bool shutdown_indicated = false;

/* Function to print debug statistics */
void cpu_stats_print_all(void) {
    if (next_func_id > MAX_FUNCS)
        fprintf(stderr, "Warning: Exceeded maximum number of tracked functions (%d)\n", MAX_FUNCS);

    printf("\nDebug Statistics Summary:\n");
    // TODO report is_on_battery, idle_prom, idle_os, shutdown_indicated
    printf("System States: Battery=%s, PROM Idle=%s, OS Idle=%s, Shutdown=%s\n",
           is_on_battery ? "On" : "Off",
           idle_prom ? "Yes" : "No",
           idle_os ? "Yes" : "No",
           shutdown_indicated ? "Yes" : "No");

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
        for (int j = 0; j < 2; j++) // True & false
            it->count[j] = it->min_streak[j] = it->max_streak[j] = it->total_ns[j] = it->min_ns[j] = it->max_ns[j] = 0;
    }
}

/* Load state edge data from file */
void load_state_edge_data() {
    FILE *f = fopen(STATE_EDGE_FILE, "rb");
    if (!f) {
        // If file doesn't exist, initialize with defaults (zeros/max values)
        // This effectively makes the first run establish the initial edges.
        // TODO min values probably should not be initialized to zero - or ensure we have a way to detect that the zero actually means uninitialized elsewhere in the code.
        memset(state_edge_data, 0, sizeof(state_edge_data));
        // Initialize min values to large numbers and max to 0? Or handle in update?
        // Let's handle it in update: if a min edge is 0, the first value becomes the min.
        // Max values start at 0, first value becomes max.
        printf("State edge file '%s' not found. Initializing defaults.\n", STATE_EDGE_FILE);
        state_edges_loaded = true; // Mark as loaded even if initialized
        return;
    }
    // Read the entire structure
    // TODO - How will we connect the function counters to the state edges? The file will need to include the func_names.
    size_t read_count = fread(state_edge_data, sizeof(struct state_edges), NUM_STATES, f);
    fclose(f);

    if (read_count != NUM_STATES) {
        fprintf(stderr, "Error reading state edge file '%s'. Read %zu states, expected %d. Using defaults.\n",
                STATE_EDGE_FILE, read_count, NUM_STATES);
        // Reset to defaults if read failed or was partial
        memset(state_edge_data, 0, sizeof(state_edge_data));
    } else {
        printf("Loaded state edge data from '%s'.\n", STATE_EDGE_FILE);
    }
    state_edges_loaded = true;
}

/* Save state edge data to file */
void save_state_edge_data() {
    if (!state_edges_loaded) {
        fprintf(stderr, "Error: Cannot save state edges, data not loaded/initialized.\n");
        return;
    }
    FILE *f = fopen(STATE_EDGE_FILE, "wb");
    if (!f) {
        perror("Error opening state edge file for writing");
        return;
    }
    // Write the entire structure
    size_t write_count = fwrite(state_edge_data, sizeof(struct state_edges), NUM_STATES, f);
    fclose(f);

    if (write_count != NUM_STATES) {
        fprintf(stderr, "Error writing state edge file '%s'. Wrote %zu states, expected %d.\n",
                STATE_EDGE_FILE, write_count, NUM_STATES);
    }
}

/* Update the min/max edges for the given state based on current counters */
void update_state_edges(int vm_state) {
    if (vm_state < 0 || vm_state >= NUM_STATES) {
        return; // Ignore invalid states like AUTODETECT
    }

    if (!state_edges_loaded) {
        load_state_edge_data();
    }

    // TODO this function need to look at all the current counters for all the functions and create/update a template (for all those functions) of max and min values for each of the counters.
    // The format of the saved file needs to include headings for each state (0, 1 and 2), and subheadings for each func_name, then the values of each variable within that function.
    // "each variable" refers to: min_streak, max_streak, avg_ns, min_ns, max_ns
    // so we need to record the edges of each of those variables (i.e. the edges of the window that they operate within - i.e. the minimum and maximum that the values reached during training per the applicable "state", i.e. idle_prom, idle_os, shutdown).

    // Note: The file format requirement seems overly complex for a simple binary save/load.
    // We are saving the raw min/max edge data directly. Reporting can format it nicely.
    // The binary format optimizes for brevity as requested.

    bool updated = false;
    struct state_edges *current_state_edges = &state_edge_data[vm_state];

    for (int i = 0; i < next_func_id; i++) {
        struct debug_counters *counters = &counters_array[i];
        struct func_state_edges *func_edges = &current_state_edges->func_edges[i];

        for (int j = 0; j < 2; j++) { // True and False stats
            // TODO - count itself also need to have edges
            uint64_t avg_ns = counters->count[j] ? counters->total_ns[j] / counters->count[j] : -1;

            // Store previous values to check if updated
            uint64_t old_min_ns_min = func_edges->min_ns_min[j];
            uint64_t old_min_ns_max = func_edges->min_ns_max[j];
            uint64_t old_max_ns_min = func_edges->max_ns_min[j];
            uint64_t old_max_ns_max = func_edges->max_ns_max[j];
            uint64_t old_avg_ns_min = func_edges->avg_ns_min[j];
            uint64_t old_avg_ns_max = func_edges->avg_ns_max[j];
            int old_min_streak_min = func_edges->min_streak_min[j];
            int old_min_streak_max = func_edges->min_streak_max[j];
            int old_max_streak_min = func_edges->max_streak_min[j];
            int old_max_streak_max = func_edges->max_streak_max[j];

            // Update edges for min/max/avg ns and min/max streak
            UPDATE_EDGE(func_edges->min_ns_min[j], func_edges->min_ns_max[j], counters->min_ns[j]);
            UPDATE_EDGE(func_edges->max_ns_min[j], func_edges->max_ns_max[j], counters->max_ns[j]);
            UPDATE_EDGE(func_edges->avg_ns_min[j], func_edges->avg_ns_max[j], avg_ns);
            // Only update streak edges if a streak occurred (count > 0 implies streak > 0)
            UPDATE_EDGE(func_edges->min_streak_min[j], func_edges->min_streak_max[j], counters->min_streak[j]);
            UPDATE_EDGE(func_edges->max_streak_min[j], func_edges->max_streak_max[j], counters->max_streak[j]);

            // Check if any edge actually changed
            // TODO - include count edges in the check
            if (old_min_ns_min != func_edges->min_ns_min[j] || old_min_ns_max != func_edges->min_ns_max[j] ||
                old_max_ns_min != func_edges->max_ns_min[j] || old_max_ns_max != func_edges->max_ns_max[j] ||
                old_avg_ns_min != func_edges->avg_ns_min[j] || old_avg_ns_max != func_edges->avg_ns_max[j] ||
                old_min_streak_min != func_edges->min_streak_min[j] || old_min_streak_max != func_edges->min_streak_max[j] ||
                old_max_streak_min != func_edges->max_streak_min[j] || old_max_streak_max != func_edges->max_streak_max[j]) {
                updated = true;
            }
        }
    }

    // Save the data if any edges were updated
    if (updated) {
        save_state_edge_data();
    }
}

/* Check if current stats fall within the defined edges for the given state */
bool is_within_state_edges(int vm_state) {
    if (vm_state < 0 || vm_state >= NUM_STATES) {
        // printf("Cannot check edges for invalid state: %d\n", vm_state);
        return true; // Or false? Let's return true to not trigger warnings for invalid states.
    }

    if (!state_edges_loaded) {
        load_state_edge_data();
    }

    struct state_edges *current_state_edges = &state_edge_data[vm_state];

    for (int i = 0; i < next_func_id; i++) {
        struct debug_counters *counters = &counters_array[i];
        struct func_state_edges *func_edges = &current_state_edges->func_edges[i];

        for (int j = 0; j < 2; j++) { // True and False stats
            uint64_t avg_ns = counters->total_ns[j] / counters->count[j];
            // Check if current values are outside the known min/max window for this state
            // Note: We compare against the min/max observed values for each stat (e.g., min_ns_min and min_ns_max)
            if (counters->min_ns[j] < func_edges->min_ns_min[j] || counters->min_ns[j] > func_edges->min_ns_max[j] ||
                counters->max_ns[j] < func_edges->max_ns_min[j] || counters->max_ns[j] > func_edges->max_ns_max[j] ||
                avg_ns < func_edges->avg_ns_min[j] || avg_ns > func_edges->avg_ns_max[j] ||
                counters->min_streak[j] < func_edges->min_streak_min[j] || counters->min_streak[j] > func_edges->min_streak_max[j] ||
                counters->max_streak[j] < func_edges->max_streak_min[j] || counters->max_streak[j] > func_edges->max_streak_max[j])
            {
                 // Log the specific counter that is out of bounds?
                printf("Warning: Stat out of bounds for func '%s', state %d, type %d\n", counters->func_name, vm_state, j);
                // Example detail: printf("  min_ns %lu not in [%lu, %lu]\n", counters->min_ns[j], func_edges->min_ns_min[j], func_edges->min_ns_max[j]);
                return false; // Found a value outside the established edges
            }
        }
    }

    return true; // All current stats are within the known edges for this state
}

void cpu_stats_per_second(void) {
    static int vm_state = STATE_AUTODETECT;
    static bool first_run = true; // Flag for initial load

    // Load state edges on the very first call
    if (first_run && !state_edges_loaded) {
        load_state_edge_data();
        first_run = false;
    }

    time_t current_time = time(NULL);
    static time_t last_stats_print = current_time;

    if (current_time == last_stats_print) return;
    last_stats_print = current_time;

    cpu_stats_print_all();
    vm_state = get_system_state(vm_state);
	update_state_edges(vm_state);
    reset_cpu_stats();
}
