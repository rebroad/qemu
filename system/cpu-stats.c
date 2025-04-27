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

// Global variables
int next_func_id = 0;
struct func_state_edges state_edge_data[NUM_STATES] = {0};
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

    // Initialize edges
    for (int s = 0; s < NUM_STATES; s++) {
        struct func_state_edges *edges = &state_edge_data[s].func_edges[func_id];
        if (func_id == -1) {
            // New function (not in save file) or data load failed: Initialize runtime edges
            memset(edges, 0, sizeof(struct func_state_edges)); // Zero out structure
            strncpy(edges->func_name, func_name, MAX_FUNC_NAME_LEN - 1);
            rt_edges->func_name[MAX_FUNC_NAME_LEN - 1] = '\0';

            for (int j = 0; j < 2; j++) {
                edges->min_ns[j][EDGE_MIN] = UINT64_MAX;
                edges->max_ns[j][EDGE_MIN] = UINT64_MAX;
                edges->avg_ns[j][EDGE_MIN] = UINT64_MAX;
                edges->min_streak[j][EDGE_MIN] = INT_MAX;
                edges->max_streak[j][EDGE_MIN] = INT_MAX;
                edges->count[j][EDGE_MIN] = INT_MAX;
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

/* Load state edge data from binary file */
static void load_state_edge_data(void) {
    printf("Attempting to load state edge data from '%s'...\n", STATE_EDGE_FILE);

    FILE *f = fopen(STATE_EDGE_FILE, "rb");
    if (!f) {
        perror("State edge file not found or cannot be opened. Initializing defaults.");
        return;
    }

    // Read header
    struct state_edge_file_header header;
    if (fread(&header, sizeof(header), 1, f) != 1) {
        fprintf(stderr, "Error reading state edge file header from '%s'. Using defaults.\n", STATE_EDGE_FILE);
        fclose(f);
        return;
    }

    // Validate header
    if (header.magic != STATE_EDGE_MAGIC || header.version != STATE_EDGE_VERSION) {
         fprintf(stderr, "Error: Invalid magic number (0x%x vs 0x%x) or version (%u vs %u) in state edge file '%s'. Using defaults.\n",
                 header.magic, STATE_EDGE_MAGIC, header.version, STATE_EDGE_VERSION, STATE_EDGE_FILE);
        fclose(f);
        return;
    }

    next_func_id = header.num_funcs_saved;
    if (!next_func_id) {
        printf("State edge file '%s' is empty or contains no function data.\n", STATE_EDGE_FILE);
        fclose(f);
        return;
    }
    if (next_func_id > MAX_DEBUG_FUNCS) {
        fprintf(stderr, "Warning: State edge file '%s' contains more functions (%u) than MAX_DEBUG_FUNCS (%d). Truncating.\n",
                STATE_EDGE_FILE, next_func_id, MAX_DEBUG_FUNCS);
        next_func_id = MAX_DEBUG_FUNCS; // Avoid overallocation
    }

    // Allocate temporary buffer for all loaded blocks
    // Size = num_funcs * num_states * sizeof(struct)
    size_t total_blocks_size = (size_t)next_func_id * NUM_STATES * sizeof(struct func_state_edges);
    state_edge_data = g_malloc(total_blocks_size);
    if (!state_edge_data) {
        fprintf(stderr, "Error allocating memory (%zu bytes) for loaded state edges. Using defaults.\n", total_blocks_size);
        fclose(f);
        next_func_id = 0;
        return;
    }

    // Read all func_state_edges blocks
    size_t blocks_to_read = (size_t)next_func_id * NUM_STATES;
    size_t blocks_read = fread(state_edge_data, sizeof(struct func_state_edges), blocks_to_read, f);

    fclose(f);

    if (blocks_read != blocks_to_read) {
        fprintf(stderr, "Error reading state edge data blocks from '%s'. Read %zu, expected %zu. Using defaults.\n", STATE_EDGE_FILE, blocks_read, blocks_to_read);
        g_free(state_edge_data);
        state_edge_data = NULL;
        next_func_id = 0;
        return;
    }

    // Create and populate the name -> func_id map
    g_func_map = g_hash_table_new_full(g_str_hash, g_str_equal,
                                      NULL, // Keys are within g_loaded_edge_blocks
                                      NULL); // Values are integers
    for (int i = 0; i < next_func_id; i++) {
        // Name should be the same across states for the same function index 'i'
        // Use the name from state 0's block
        const char *func_name = state_edge_data[i * NUM_STATES + 0].func_name;
        // Ensure null termination from file read, just in case
        ((char*)func_name)[MAX_FUNC_NAME_LEN - 1] = '\0';

        if (strlen(func_name) > 0)
            g_hash_table_insert(g_func_map, (gpointer)func_name, GINT_TO_POINTER(i));
        else
            fprintf(stderr, "Warning: Empty function name found at saved index %d in '%s'. Skipping mapping.\n", i, STATE_EDGE_FILE);
    }

    printf("Loaded %d function edge sets from '%s'.\n", next_func_id, STATE_EDGE_FILE);
    state_edges_loaded = true;
}

/* Save state edge data to file */
void save_state_edge_data() {
    FILE *f = fopen(STATE_EDGE_FILE, "wb");
    if (!f) {
        perror("Error opening state edge file for writing");
        return;
    }

    // Write header
    struct state_edge_file_header header = {
        .magic = STATE_EDGE_MAGIC,
        .version = STATE_EDGE_VERSION,
        .num_funcs_saved = (uint32_t)next_func_id // Save current number of functions
    };
    if (fwrite(&header, sizeof(header), 1, f) != 1) {
        fprintf(stderr, "Error writing state edge file header to '%s'. Aborting save.\n", STATE_EDGE_FILE);
        fclose(f);
        return;
    }

    // Write data blocks: Loop through functions, then states
    size_t blocks_written = 0;
    for (int i = 0; i < next_func_id; i++) {
        for (int s = 0; s < NUM_STATES; s++) {
            struct func_state_edges *edges = &state_edge_data[s].func_edges[i];

            if (fwrite(edges, sizeof(struct func_state_edges), 1, f) == 1)
                blocks_written++;
            else {
                fprintf(stderr, "Error writing state edge data block (func_id %d, state %d) to '%s'. Aborting save.\n", i, s, STATE_EDGE_FILE);
                fclose(f);
                return;
            }
        }
    }

    // Write the entire structure
    //size_t write_count = fwrite(state_edge_data, sizeof(struct func_state_edges), NUM_STATES, f);
    fclose(f);

    size_t expected_blocks = (size_t)next_func_id * NUM_STATES;
    if (blocks_written != expected_blocks)
        fprintf(stderr, "Error saving state edge data: Wrote %zu blocks, expected %zu.\n",
                blocks_written, expected_blocks);
}

// Inline helper to update min/max for uint64_t edges
static inline void update_u64_edge(uint64_t edge[2], uint64_t current_val) {
    if (current_val < edge[EDGE_MIN]) edge[EDGE_MIN] = current_val;
    if (current_val > edge[EDGE_MAX]) edge[EDGE_MAX] = current_val;
}

// Inline helper to update min/max for int edges
static inline void update_int_edge(int edge[2], int current_val) {
    if (current_val < edge[EDGE_MIN]) edge[EDGE_MIN] = current_val;
    if (current_val > edge[EDGE_MAX]) edge[EDGE_MAX] = current_val;
}


/* Update the min/max edges for the given state based on current counters */
static void update_state_edges(int vm_state) {
    if (vm_state < 0 || vm_state >= NUM_STATES) return;

    bool updated = false;
    struct state_edges *state_edges = &state_edge_data[vm_state];

    for (int i = 0; i < next_func_id; i++) {
        struct debug_counters *counters = &counters_array[i];
        struct func_state_edges *edges = &state_edges->func_edges[i];

        // Ensure names match - defensive check
        if (strncmp(edges->func_name, counters->func_name, MAX_FUNC_NAME_LEN)) {

            fprintf(stderr, "Warning: Name mismatch in update_state_edges for index %d ('%s' vs '%s')\n",
                     i, func_edges->func_name, counters->func_name);
            // Ensure name is correct in the edge structure
            strncpy(func_edges->func_name, counters->func_name, MAX_FUNC_NAME_LEN - 1);
            func_edges->func_name[MAX_FUNC_NAME_LEN - 1] = '\0';
            // continue; // Maybe skip update if names mismatched? Or just fix name? Let's fix and continue.
        }

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
