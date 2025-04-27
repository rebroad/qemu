#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include "cpu-stats.h"

#define STATE_EDGE_FILE "state_edges.dat"
#define MAX_FUNC_NAME_LEN 64 // Max length for function names
#define STATE_EDGE_MAGIC 0xCPU57A75 // Magic number for file format
#define STATE_EDGE_VERSION 1        // File format version

// System state flags
#define STATE_AUTODETECT  -1
#define STATE_PROM_IDLE   0
#define STATE_OS_IDLE     1
#define STATE_SHUTDOWN    2
#define NUM_STATES        3  // PROM_IDLE, OS_IDLE, SHUTDOWN

// Structure to hold the min/max edges for counters per function
// This structure is used both in the save file and runtime
struct func_state_edges {
    char func_name[MAX_FUNC_NAME_LEN]; // Function name associated with these edges
    // Format: stat[true/false][min/max]
    uint64_t min_ns[2][2];
    uint64_t max_ns[2][2];
    uint64_t avg_ns[2][2];
    int min_streak[2][2];
    int max_streak[2][2];
    int count[2][2]; // Min/Max observed value for count[0/1]
} func_edges[MAX_DEBUG_FUNCS];

// Header structure for the state edge file
struct state_edge_file_header {
    uint32_t magic;
    uint32_t version;
    uint32_t num_funcs_saved;
};

// Global variables for debug counters
int next_func_id = 0;

// Global variables for state edges
struct state_edges state_edge_data[NUM_STATES] = {0};
bool state_edges_loaded = false;

// Globals used during loading/mapping
GHashTable *g_func_map = NULL;

// Placeholders for system state variables - TODO: Might not need these
bool idle_prom = false;
bool idle_os = false;
bool shutdown_indicated = false;

// Function to print debug statistics
void cpu_stats_print_all(void) {
    if (next_func_id >= MAX_FUNCS)
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

// Find (or create) the counters struct for a function
struct debug_counters *find_counters_array(const char *func_name) {
    if (next_func_id >= MAX_DEBUG_FUNCS) return NULL;

    // Ensure the main counters array is allocated
    if (!counters_array) {
        counters_array = g_new0(struct debug_counters, MAX_DEBUG_FUNCS);
		g_func_map = g_hash_table_new_full(g_str_hash, g_str_equal,
                                           NULL,   // No need to free keys (__func__)
                                           NULL);  // No need to free values (int)
        load_state_edge_data();
		ensure_maps_initialized();
    }

    gpointer func_id_ptr = NULL;
    int func_id = -1;
    // Check the map populated by load_state_edge_data
    if (state_edges_loaded &&
        g_hash_table_lookup_extended(g_func_map, func_name, NULL, &func_id_ptr))
    {
        func_id = GPOINTER_TO_INT(func_id_ptr);
    }

    // Initialize runtime edges (either from loaded data or defaults)
    for (int s = 0; s < NUM_STATES; s++) {
        struct func_state_edges *rt_edges = &state_edge_data[s].func_edges[func_id];
        if (func_id == -1) {
            // New function (not in save file) or data load failed: Initialize runtime edges
            memset(rt_edges, 0, sizeof(struct func_state_edges)); // Zero out structure
            strncpy(rt_edges->func_name, func_name, MAX_FUNC_NAME_LEN - 1);
            rt_edges->func_name[MAX_FUNC_NAME_LEN - 1] = '\\0';

            for (int j = 0; j < 2; j++) {
                rt_edges->min_ns[j][EDGE_MIN] = UINT64_MAX;
                rt_edges->max_ns[j][EDGE_MIN] = UINT64_MAX;
                rt_edges->avg_ns[j][EDGE_MIN] = UINT64_MAX;
                rt_edges->min_streak[j][EDGE_MIN] = INT_MAX;
                rt_edges->max_streak[j][EDGE_MIN] = INT_MAX;
                rt_edges->count[j][EDGE_MIN] = INT_MAX;
            }
        }
    }

    if (func_id < 0)
        func_id = next_func_id++; // Assign the next available ID
    // TODO - need to ensure next_func_id is updated in load_state_edge_data()

    return &counters_array[func_id];
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
        // Initialize min values to large numbers and max to 0? Or handle in update?
        // Let's handle it in update: if a min edge is 0, the first value becomes the min.
        // Max values start at 0, first value becomes max.
        printf("State edge file '%s' not found. Initializing defaults.\n", STATE_EDGE_FILE);
    } else {
        // Read the entire structure
        // TODO - How will we connect the function counters to the state edges? The file will need to include the func_names.
        size_t read_count = fread(state_edge_data, sizeof(struct state_edges), NUM_STATES, f);
        fclose(f);

        if (read_count != NUM_STATES) {
            fprintf(stderr, "Error reading state edge file '%s'. Read %zu states, expected %d. Using defaults.\n",
                    STATE_EDGE_FILE, read_count, NUM_STATES);
        } else {
		    printf("Loaded state edge data from '%s'.\n", STATE_EDGE_FILE);
		    state_edges_loaded = true;
			// TODO - update next_func_id based on how many functions loaded
			// TODO - g_func_map should also be populated by what we load here
		}
	}
}

/* Save state edge data to file */
void save_state_edge_data() {
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

    // TODO this function need to look at all the current counters for all the functions and create/update a template (for all those functions) of max and min values for each of the counters.

    bool updated = false;
    struct state_edges *current_state_edges = &state_edge_data[vm_state];

    for (int i = 0; i < next_func_id; i++) {
        struct debug_counters *counters = &counters_array[i];
        struct func_state_edges *func_edges = &current_state_edges->func_edges[i];

        for (int j = 0; j < 2; j++) { // True and False stats
            // TODO - count itself also need to have edges
            uint64_t avg_ns = counters->count[j] ? counters->total_ns[j] / counters->count[j] : -1;

			// TODO loop through min/max (0/1) array for below
            // Store previous values to check if updated
            // TODO - include count edges in the check
			// TODO - this function!
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

    time_t current_time = time(NULL);
    static time_t last_stats_print = current_time;

    if (current_time == last_stats_print) return;
    last_stats_print = current_time;

    cpu_stats_print_all();
    vm_state = get_system_state(vm_state);
    update_state_edges(vm_state);
    reset_cpu_stats();
}
