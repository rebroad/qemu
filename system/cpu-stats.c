#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include "cpu-stats.h"
#include "qemu/timer.h" // Include for QEMU timers

#define STATE_EDGE_FILE "state_edges.dat"
#define MAX_FUNC_NAME_LEN 64 // Max length for function names
#define STATE_EDGE_MAGIC 0x43505557 // Magic number for file format (CPUW)
#define STATE_EDGE_VERSION 1        // File format version

// System state flags
#define STATE_AUTODETECT       -1
#define STATE_PROM_IDLE        0
#define STATE_OS_IDLE          1
#define STATE_SHUTDOWN         2
#define NUM_BASE_STATES        3

// System state flags for AC and Battery
#define STATE_PROM_IDLE_AC     (STATE_PROM_IDLE * 2)
#define STATE_PROM_IDLE_BAT    (STATE_PROM_IDLE * 2 + 1)
#define STATE_OS_IDLE_AC       (STATE_OS_IDLE * 2)
#define STATE_OS_IDLE_BAT      (STATE_OS_IDLE * 2 + 1)
#define STATE_SHUTDOWN_AC      (STATE_SHUTDOWN * 2)
#define STATE_SHUTDOWN_BAT     (STATE_SHUTDOWN * 2 + 1)

#define NUM_STATES             (NUM_BASE_STATES * 2)

// Structure to hold the min/max edges for counters per function
// This structure is used both in the save file and runtime
struct func_state_edges {
    // Format: stat[min/max] (no true/false)
    uint64_t call_count[2];
    // Format: stat[true/false][min/max]
    uint64_t min_ns[2][2];
    uint64_t max_ns[2][2];
    uint64_t avg_ns[2][2];
    int min_streak[2][2];
    int max_streak[2][2];
    uint64_t count[2][2]; // Min/Max observed value for count[0/1]
} func_edges[MAX_DEBUG_FUNCS];

// Header structure for the state edge file
struct state_edge_file_header {
    uint32_t magic;
    uint32_t version;
    uint32_t num_funcs_saved;
};

// Forward declarations
static void load_state_edge_data(void);
static bool detect_system_state(int vm_state);
static void save_state_edge_data(void);

// Global variables for debug counters
int next_func_id = 0;
struct debug_counters *counters_array = NULL; // Memory allocated on first use

// Global variables for state edges
struct func_state_edges state_edge_data[NUM_STATES][MAX_DEBUG_FUNCS] = {0}; // TODO dynamically allocate?
bool state_edges_loaded = false;

// Globals used during loading/mapping
GHashTable *g_func_map = NULL;
static QEMUTimer *cpu_stats_timer = NULL; // Timer for periodic stats update

static bool is_on_battery(void) {
    FILE *f = fopen("/sys/class/power_supply/ACAD/online", "r");
    if (!f) return false;

    char status;
    bool on_battery = (fscanf(f, "%c", &status) == 1 && status == '0');
    fclose(f);
    return on_battery;
}

// Function to print debug statistics
static void cpu_stats_print_all(void) {
    if (next_func_id >= MAX_DEBUG_FUNCS)
        fprintf(stderr, "Warning: Exceeded maximum number of tracked functions (%d)\n", MAX_DEBUG_FUNCS);

    bool on_battery = is_on_battery();
    bool prom_idle = detect_system_state(STATE_PROM_IDLE * 2 + on_battery);
    bool os_idle = detect_system_state(STATE_OS_IDLE * 2 + on_battery);
    bool shutdown_indicated = detect_system_state(STATE_SHUTDOWN * 2 + on_battery);

    printf("\nSystem States: Power=%s%s%s%s\n",
           on_battery ? "Battery" : "AC", prom_idle ? " prom_idle" : "",
           os_idle ? " os_idle" : "", shutdown_indicated ? " shutdown" : "");

    for (int i = 0; i < next_func_id; i++) {
        struct debug_counters *it = &counters_array[i];
        if (it->count[0] == 0 && it->count[1] == 0)
            printf("%s [%s] (thread=%lu): called %d times\n",
                   it->func_name, it->file_name, (unsigned long)it->thread_id, it->call_count);
        else {
            uint64_t avg_false_ns = it->count[0] ? it->total_ns[0] / it->count[0] : -1;
            uint64_t avg_true_ns = it->count[1] ? it->total_ns[1] / it->count[1] : -1;
            printf("%s [%s] (thread=%lu): true=%d (avg/min/max=%" PRIu64 "/%" PRIu64 "/%" PRIu64 " ns, streak=%d-%d), "
                   "false=%d (avg/min/max=%" PRIu64 "/%" PRIu64 "/%" PRIu64 " ns, streak=%d-%d)\n",
                   it->func_name, it->file_name, (unsigned long)it->thread_id,
                   it->count[1], avg_true_ns, it->min_ns[1], it->max_ns[1],
                   it->min_streak[1], it->max_streak[1],
                   it->count[0], avg_false_ns, it->min_ns[0], it->max_ns[0],
                   it->min_streak[0], it->max_streak[0]);
        }
    }
}

/* Timer callback function */
static void cpu_stats_timer_cb(void *opaque)
{
    cpu_stats_per_second(); // Call the original periodic function
    // Re-arm the timer for the next second
    timer_mod(cpu_stats_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
}

// Find (or create) the counters struct for a function
struct debug_counters *find_counters_array(const char *func_name, const char *file_name, pthread_t thread_id) {
    if (next_func_id >= MAX_DEBUG_FUNCS) return NULL;

    // Ensure the main counters array is allocated and timer is initialized
    if (!counters_array) {
        counters_array = g_new0(struct debug_counters, MAX_DEBUG_FUNCS);
        // Initialize hash map with NULL key_destroy_func since we're using string literals
        g_func_map = g_hash_table_new_full(g_str_hash, g_str_equal,
                                           NULL,   // Don't free the keys (they're string literals)
                                           NULL);  // Values (func_id) are integers

        // Initialize and arm the periodic timer
        cpu_stats_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, cpu_stats_timer_cb, NULL);
        timer_mod(cpu_stats_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000); // Fire in 1 sec

        load_state_edge_data(); // Load data which populates g_func_map and state_edge_data
    }

    gpointer func_id_ptr = NULL;
    int func_id = -1; bool new_func = false;
    // Check the map populated by load_state_edge_data
    if (state_edges_loaded &&
        g_hash_table_lookup_extended(g_func_map, func_name, NULL, &func_id_ptr))
    {
        func_id = GPOINTER_TO_INT(func_id_ptr);
        // Check if this is a new thread calling the same function
        if (counters_array[func_id].thread_id != thread_id) {
            new_func = true;
            func_id = next_func_id++; // Assign the next available ID
        } else {
            fprintf(stderr, "Found existing function %s from %s at index %d (thread=%lu)\n",
                    func_name, file_name, func_id, (unsigned long)thread_id);
        }
    } else {
        new_func = true;
        func_id = next_func_id++; // Assign the next available ID
    }

    if (new_func) {
        // Store the function name and thread ID in the main counters array
        counters_array[func_id].func_name = func_name;
        counters_array[func_id].file_name = file_name;
        counters_array[func_id].thread_id = thread_id;

        // Also add it to the map for future lookups
        g_hash_table_insert(g_func_map, (gpointer)func_name, GINT_TO_POINTER(func_id));
        fprintf(stderr, "New func %s from %s at idx %d (%p,%lu) counters=%p\n",
                func_name, file_name, func_id, (void*)func_name, (unsigned long)thread_id,
                (void*)&counters_array[func_id]);
    }

    // Initialize runtime state edges if this is a newly encountered function
    // (either truly new or loaded but not yet initialized by this function)
    if (new_func) {
        // Initialize edges for the new function
        for (int s = 0; s < NUM_STATES; s++) {
            struct func_state_edges *edges = &state_edge_data[s][func_id];

            // Initialize with max/min sentinels
            edges->call_count[0] = UINT64_MAX;
            edges->call_count[1] = 0;
            for (int j = 0; j < 2; j++) { // True/False
                edges->min_ns[j][0] = UINT64_MAX; edges->min_ns[j][1] = 0;
                edges->max_ns[j][0] = 0; edges->max_ns[j][1] = 0; // Max starts at 0
                edges->avg_ns[j][0] = UINT64_MAX; edges->avg_ns[j][1] = 0;
                edges->min_streak[j][0] = INT_MAX; edges->min_streak[j][1] = 0;
                edges->max_streak[j][0] = 0; edges->max_streak[j][1] = 0; // Max starts at 0
                edges->count[j][0] = UINT64_MAX; edges->count[j][1] = 0;
            }
        }
    }

    return &counters_array[func_id];
}

/* Get system state from external source (e.g., file) */
static int get_system_state(int current_state) {
    int base_state = STATE_AUTODETECT;
    // TODO: Define vm_state.txt path properly
    FILE *state_file = fopen("vm_state.txt", "r");
    if (state_file) {
        if (fscanf(state_file, "%d", &base_state) != 1) {
            fprintf(stderr, "Warning: Failed to read state from vm_state.txt. Using autodetect.\n");
            base_state = STATE_AUTODETECT;
        } else if (base_state < 0 || base_state >= NUM_BASE_STATES) {
            fprintf(stderr, "Warning: Invalid base state %d read from vm_state.txt. Using autodetect.\n", base_state);
            base_state = STATE_AUTODETECT;
        }
        fclose(state_file);
    }

    if (base_state == STATE_AUTODETECT) return base_state;

    int new_state = base_state * 2 + (is_on_battery() ? 1 : 0);

    if (new_state != current_state)
        printf("State change detected: %d -> %d\n", current_state, new_state);

    return new_state;
}

static void reset_cpu_stats(void) {
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
        fprintf(stderr, "No state edge data found.\n");
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

    int num_funcs_loaded = header.num_funcs_saved;
    if (!num_funcs_loaded) {
        printf("State edge file '%s' is empty or contains no function data.\n", STATE_EDGE_FILE);
        fclose(f);
        return;
    }
    if (num_funcs_loaded > MAX_DEBUG_FUNCS) {
        fprintf(stderr, "Warning: State edge file '%s' contains more functions (%u) than MAX_DEBUG_FUNCS (%d). Truncating.\n",
                STATE_EDGE_FILE, num_funcs_loaded, MAX_DEBUG_FUNCS);
        num_funcs_loaded = MAX_DEBUG_FUNCS; // Avoid overreading or overallocating later
    }

    // Assume g_func_map is already initialized by the caller (find_counters_array)
    if (!g_func_map) {
        fprintf(stderr, "Error: g_func_map not initialized before loading state edges.\n");
        fclose(f);
        return;
    }

    // Read function names and state data
    for (int i = 0; i < num_funcs_loaded; i++) {
        char func_name_buf[MAX_FUNC_NAME_LEN];
        if (fread(func_name_buf, sizeof(char), MAX_FUNC_NAME_LEN, f) != MAX_FUNC_NAME_LEN) {
            fprintf(stderr, "Error reading function name for index %d from '%s'. Load aborted.\\n", i, STATE_EDGE_FILE);
            fclose(f);
            // Clean up potentially partially populated map/data?
            g_hash_table_remove_all(g_func_map); // Clear map on error
            next_func_id = 0; // Reset func count
            return;
        }
        func_name_buf[MAX_FUNC_NAME_LEN - 1] = '\0'; // Ensure null termination

        // Map the name to the function index 'i'
        if (strlen(func_name_buf) > 0) {
            gchar *name_key = g_strdup(func_name_buf);
            g_hash_table_insert(g_func_map, name_key, GINT_TO_POINTER(i));
            // Note: name_key is now owned by the hash table if g_str_hash/equal are used
            // with a key_destroy_func=g_free in g_hash_table_new_full.
            // If default funcs used, we need to manage this memory or leak it.
            // Assuming g_hash_table_new_full uses g_free for keys.
        } else {
            fprintf(stderr, "Warning: Empty function name found at saved index %d in '%s'. Skipping mapping.\n", i, STATE_EDGE_FILE);
            // Skip reading state data for this empty name entry? Or read and discard?
            // Let's read and discard to keep file position correct.
            size_t dummy_read_size = sizeof(struct func_state_edges) * NUM_STATES;
            if (fseek(f, dummy_read_size, SEEK_CUR) != 0) {
                 fprintf(stderr, "Error seeking past state data for empty name at index %d.\n", i);
                 fclose(f);
                 g_hash_table_remove_all(g_func_map);
                 next_func_id = 0;
                 return;
            }
            continue; // Skip to next function
        }

        // Read state data for this function
        for (int s = 0; s < NUM_STATES; s++) {
            // Read directly into the destination structure
            if (fread(&state_edge_data[s][i], sizeof(struct func_state_edges), 1, f) != 1) {
                fprintf(stderr, "Error reading state edge data for func_id %d, state %d from '%s'. Load aborted.\\n", i, s, STATE_EDGE_FILE);
                fclose(f);
                g_hash_table_remove_all(g_func_map);
                next_func_id = 0;
                return;
            }
        }
    }

    fclose(f);

    next_func_id = num_funcs_loaded; // Update global count based on successful load
    printf("Loaded %d function edge sets for %d states from '%s'.\\n", next_func_id, NUM_STATES, STATE_EDGE_FILE);
    state_edges_loaded = true;
}

/* Save state edge data to binary file */
static void save_state_edge_data(void) {
    FILE *f = fopen(STATE_EDGE_FILE, "wb");
    if (!f) {
        perror("Could not create state edge file for writing");
        return;
    }

    // Write header
    struct state_edge_file_header header = {
        .magic = STATE_EDGE_MAGIC,
        .version = STATE_EDGE_VERSION,
        .num_funcs_saved = (uint32_t)next_func_id // Save current number of functions
    };
    if (fwrite(&header, sizeof(header), 1, f) != 1) {
        fprintf(stderr, "Error writing state edge file header to '%s'.\n", STATE_EDGE_FILE);
        fclose(f);
        return;
    }

    // Write data: Loop functions, then states
    for (int i = 0; i < next_func_id; i++) {
        // Assume counters_array is populated correctly and name exists
        // Write function name (fixed size)
        if (fwrite(counters_array[i].func_name, sizeof(char), MAX_FUNC_NAME_LEN, f) != MAX_FUNC_NAME_LEN) {
             fprintf(stderr, "Error writing function name for index %d to '%s'. File may be corrupted.\n", i, STATE_EDGE_FILE);
             fclose(f);
             return;
        }

        // Write state data for this function
        for (int s = 0; s < NUM_STATES; s++) {
            // Write directly from the source structure
            if (fwrite(&state_edge_data[s][i], sizeof(struct func_state_edges), 1, f) != 1) {
                fprintf(stderr, "Error writing state edge data for func_id %d, state %d to '%s'. File may be corrupted.\\n", i, s, STATE_EDGE_FILE);
                fclose(f);
                return; // Stop writing on error
            }
        }
    }

    fclose(f);
    printf("Saved %d function edge sets for %d states to '%s'.\n", next_func_id, NUM_STATES, STATE_EDGE_FILE);
}

#define CHECK_EDGE(edge, current_val) \
    do { \
        if (current_val < edge[0]) { \
            if (update) edge[0] = current_val; \
            outside = true; \
        } \
        if (current_val > edge[1]) { \
            if (update) edge[1] = current_val; \
            outside = true; \
        } \
    } while (0)

/* Update the min/max edges for the given state based on current counters */
static bool state_edges(int vm_state, bool update) {
    if (vm_state < 0 || vm_state >= NUM_STATES) return false;

    bool outside = false;

    for (int i = 0; i < next_func_id; i++) {
        struct debug_counters *counters = &counters_array[i];
        struct func_state_edges *edges = &state_edge_data[vm_state][i];

        CHECK_EDGE(edges->call_count, counters->call_count);
        for (int j = 0; j < 2; j++) { // True=1 and False=0 stats
            // Only update edges if the counter was actually hit in this interval
            if (counters->count[j] > 0) {
                // Calculate average for this interval
                uint64_t avg_ns = counters->total_ns[j] / counters->count[j];
                CHECK_EDGE(edges->min_ns[j], counters->min_ns[j]);
                CHECK_EDGE(edges->max_ns[j], counters->max_ns[j]);
                CHECK_EDGE(edges->avg_ns[j], avg_ns);
                CHECK_EDGE(edges->min_streak[j], counters->min_streak[j]);
                CHECK_EDGE(edges->max_streak[j], counters->max_streak[j]);
                CHECK_EDGE(edges->count[j], counters->count[j]);
            }
        }
    }

    // Save the data file if any edges were updated
    if (update && outside) save_state_edge_data();

    return !outside;
}

static void update_state_edges(int vm_state) { state_edges(vm_state, true); }

static bool detect_system_state(int vm_state) { return state_edges(vm_state, false); }

/* Called periodically by the timer */
void cpu_stats_per_second(void) {
    static int vm_state = STATE_AUTODETECT;

    cpu_stats_print_all();
    vm_state = get_system_state(vm_state);
    if (vm_state != STATE_AUTODETECT) update_state_edges(vm_state);
    reset_cpu_stats();
}
