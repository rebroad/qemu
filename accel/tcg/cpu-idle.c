/*
 * QEMU CPU Idle Detection and Power Saving
 *
 * Architecture-agnostic idle loop detection using PC pattern learning.
 * Currently used by SPARC, designed to be extensible to all architectures.
 *
 * Copyright (c) 2024 QEMU Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"  /* For vCPU wait time stats */
#include "exec/cpu-defs.h"  /* For target_ulong */
#include "hw/core/cpu.h"
#include "qom/object.h"
#include "system/cpu-idle.h"
#include "system/cpu-timers.h"
#include "system/cpu-throttle.h"
#include "util/time-format.h"
#include "monitor/monitor.h"
#include "monitor/hmp-target.h"
#include "qapi/qmp/qdict.h"
#include <sys/stat.h>

/* MAX_ICOUNT_SHIFT from icount-common.c */
#define MAX_ICOUNT_SHIFT 10

/* Global control flags */
static bool cpu_idle_enabled = false;  // Controls whether idle detection is active
static bool cpu_idle_debug = false;    // Controls debug output

/* Debug output helper - use stderr to not interfere with serial console */
#define DEBUG_PRINTF(...) do { if (cpu_idle_debug) { fprintf(stderr, __VA_ARGS__); fflush(stderr); } } while(0)

/* PC learning system for idle detection */
typedef enum {
    LEARNING_OFF = 0,
    LEARNING_PROM_IDLE,
    LEARNING_OS_IDLE,      // Renamed from OS_IDLE to be generic
    LEARNING_BUSY
} LearningMode;

#define NUM_COLLECTIONS 3  // PROM_IDLE, OS_IDLE, BUSY

static LearningMode learning_mode  = LEARNING_OFF;

#define MAX_PC_CANDIDATES 1000
#define LEARNING_AUTO_STOP_SAMPLES 10000  // Auto-stop learning after 10K samples

/* PC candidate structure */
typedef struct {
    uint64_t pc;
    uint64_t npc;
    uint32_t count;           // Raw count from learning
    uint32_t effective_count; // Confidence-adjusted count
} PCCandidate;

/* PC collection structure */
typedef struct {
    PCCandidate pcs[MAX_PC_CANDIDATES];
    int num_pcs;
    uint32_t total_samples;
    const char *name;
    LearningMode mode;
    int max_consecutive_repeats;  // Track longest PC/NPC repeat sequence
} PCCollection;

/* Learning collections (array index = mode - 1, since OFF has no collection) */
static PCCollection collections[NUM_COLLECTIONS] = {
    {.name = "PROM-IDLE", .mode = LEARNING_PROM_IDLE},
    {.name = "OS-IDLE", .mode = LEARNING_OS_IDLE},
    {.name = "BUSY", .mode = LEARNING_BUSY}
};

/* Runtime-configurable settings */
static int max_sleep_us_cap = 10000;  // Max sleep cap (from config file)
static int current_sleep_cap = 10000; // Dynamically adjusted based on time sync
static time_t last_config_mtime = 0;   // Track config file mtime for auto-reload
static bool auto_tune_sleep = true;    // Auto-adjust sleep to maintain time sync

/* Helper: Extract architecture name from CPU typename (e.g., "sparc-cpu" → "sparc") */
static const char *get_arch_name_from_cpu(CPUState *cpu)
{
    static char arch_name[32];
    const char *typename = object_get_typename(OBJECT(cpu));

    // Extract base name before "-cpu" suffix
    const char *dash = strstr(typename, "-cpu");
    if (dash) {
        size_t len = dash - typename;
        if (len < sizeof(arch_name)) {
            memcpy(arch_name, typename, len);
            arch_name[len] = '\0';
            return arch_name;
        }
    }

    // Fallback: use full typename
    snprintf(arch_name, sizeof(arch_name), "%s", typename);
    return arch_name;
}

/* Helper: Get save filename based on architecture and icount mode */
static inline const char *get_idle_pc_save_file(const char *arch_name)
{
    static char filename[256];
    const char *suffix = icount_enabled() ? "-icount" : "";
    snprintf(filename, sizeof(filename), "qemu-%s-idle-pcs%s.dat", arch_name, suffix);
    return filename;
}

// Helper: open file in CWD or fallback to /tmp
static FILE *fopen_with_tmp_fallback(const char *filename, const char *mode)
{
    FILE *f = fopen(filename, mode);
    if (!f) {
        char tmp_path[512];  // Large enough for /tmp/ prefix + max filename
        snprintf(tmp_path, sizeof(tmp_path), "/tmp/%s", filename);
        f = fopen(tmp_path, mode);
    }
    return f;
}

// Helper: stat file in CWD or fallback to /tmp
static int stat_with_tmp_fallback(const char *filename, struct stat *st)
{
    if (stat(filename, st) == 0) {
        return 0;  // Success in CWD
    }
    // Try /tmp
    char tmp_path[512];  // Large enough for /tmp/ prefix + max filename
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/%s", filename);
    return stat(tmp_path, st);
}

// Recalculate effective_count for all idle PCs using Bayesian adjustment
// Call this after loading data or after BUSY learning completes
static void recalculate_effective_counts(void)
{
    PCCollection *busy_coll = &collections[LEARNING_BUSY - 1];

    // Process both idle collections (PROM and OS)
    for (int idle_type = 0; idle_type < 2; idle_type++) {
        PCCollection *idle_coll = &collections[idle_type];

        for (int i = 0; i < idle_coll->num_pcs; i++) {
            // Find if this PC exists in BUSY collection
            uint32_t busy_count = 0;
            if (busy_coll->num_pcs > 0 && busy_coll->total_samples > 0) {
                for (int b = 0; b < busy_coll->num_pcs; b++) {
                    if (busy_coll->pcs[b].pc == idle_coll->pcs[i].pc &&
                        busy_coll->pcs[b].npc == idle_coll->pcs[i].npc) {
                        busy_count = busy_coll->pcs[b].count;
                        break;
                    }
                }
            }

            // Confidence-based adjustment: compare frequency in idle vs busy
            // Smooth scaling based on how much more frequent in idle vs busy
            // multiplier = (idle_rate / busy_rate) - 1.0 gives:
            //   idle_rate = busy_rate    → 0x (eliminate, appears equally)
            //   idle_rate = 1.5×busy_rate → 0.5x (slight boost)
            //   idle_rate = 2×busy_rate   → 1x (keep original)
            //   idle_rate = 3×busy_rate   → 2x (double)
            //   idle_rate >> busy_rate    → large boost
            if (busy_count > 0) {
                // Calculate frequency rates (appearances per sample)
                double idle_rate = (double)idle_coll->pcs[i].count / idle_coll->total_samples;
                double busy_rate = (double)busy_count / busy_coll->total_samples;

                // Smooth multiplier: subtract 1 to center around 1x when idle is 2x busy
                double multiplier = (idle_rate / busy_rate) - 1.0;

                if (multiplier <= 0.0) {
                    // Appears equally or more in busy → eliminate
                    idle_coll->pcs[i].effective_count = 0;
                } else {
                    // Boost proportionally
                    double effective = (double)idle_coll->pcs[i].count * multiplier;

                    // Prevent overflow
                    if (effective > UINT32_MAX) {
                        idle_coll->pcs[i].effective_count = UINT32_MAX;
                    } else {
                        idle_coll->pcs[i].effective_count = (uint32_t)effective;
                    }
                }
            } else {
                // No busy contamination - maximum confidence (set to UINT32_MAX)
                idle_coll->pcs[i].effective_count = UINT32_MAX;
            }
        }
    }
}

// Save/load learned PCs to/from disk
// Comparator for sorting PC candidates: count DESC, pc ASC, npc ASC
static int compare_pc_candidates(const void *a, const void *b)
{
    const PCCandidate *ca = (const PCCandidate *)a;
    const PCCandidate *cb = (const PCCandidate *)b;

    // Primary: count descending (higher counts first)
    if (ca->count != cb->count) {
        return (ca->count > cb->count) ? -1 : 1;
    }

    // Secondary: PC ascending
    if (ca->pc != cb->pc) {
        return (ca->pc < cb->pc) ? -1 : 1;
    }

    // Tertiary: NPC ascending
    if (ca->npc != cb->npc) {
        return (ca->npc < cb->npc) ? -1 : 1;
    }

    return 0;
}

static void save_learned_pcs(const char *arch_name)
{
    const char *filename = get_idle_pc_save_file(arch_name);
    FILE *f = fopen_with_tmp_fallback(filename, "w");
    if (!f) {
        DEBUG_PRINTF("⚠️  Failed to save learned PCs\n");
        return;
    }

    // Sort each collection before saving (count DESC, pc ASC, npc ASC)
    for (int m = 0; m < NUM_COLLECTIONS; m++) {
        PCCollection *coll = &collections[m];
        if (coll->num_pcs > 0) {
            qsort(coll->pcs, coll->num_pcs, sizeof(PCCandidate), compare_pc_candidates);
        }
    }

    // Save format: mode num_pcs max_consecutive_repeats total_samples [pc npc count]...
    for (int m = 0; m < NUM_COLLECTIONS; m++) {  // Save ALL collections (PROM, OS, BUSY)
        PCCollection *coll = &collections[m];
        fprintf(f, "%d %d %d %u\n", coll->mode, coll->num_pcs, coll->max_consecutive_repeats, coll->total_samples);
        for (int i = 0; i < coll->num_pcs; i++) {
            fprintf(f, "0x%lx 0x%lx %u\n",
                    (unsigned long)coll->pcs[i].pc,
                    (unsigned long)coll->pcs[i].npc,
                    coll->pcs[i].count);
        }
    }

    // Save max_sleep_us_cap (runtime-configurable)
    fprintf(f, "MAX_SLEEP_US %d\n", max_sleep_us_cap);

    fclose(f);
    DEBUG_PRINTF("💾 Saved learned PCs to %s\n", filename);

    // Update mtime to prevent immediate reload of our own save
    struct stat st;
    if (stat_with_tmp_fallback(filename, &st) == 0) {
        last_config_mtime = st.st_mtime;
    }

    fflush(stdout);
}

static void load_learned_pcs(const char *arch_name)
{
    const char *filename = get_idle_pc_save_file(arch_name);
    FILE *f = fopen_with_tmp_fallback(filename, "r");
    if (!f) {
        return;  // No saved file, use hardcoded defaults
    }

    DEBUG_PRINTF("📂 Loading learned PCs from %s%s...\n", filename,
                icount_enabled() ? " [ICOUNT MODE]" : "");

    int mode, num_pcs, max_repeats;
    unsigned int total_samples;
    char line[256];

    while (fgets(line, sizeof(line), f)) {
        // Check for THRESHOLD line (legacy - ignored, now using BUSY collection directly)
        if (sscanf(line, "THRESHOLD %d", &max_repeats) == 1) {
            DEBUG_PRINTF("   Ignored legacy THRESHOLD line (now using BUSY collection)\n");
            continue;
        }

        // Check for MAX_SLEEP_US line (runtime-configurable cap)
        int sleep_cap;
        if (sscanf(line, "MAX_SLEEP_US %d", &sleep_cap) == 1) {
            max_sleep_us_cap = sleep_cap;
            DEBUG_PRINTF("   Loaded max_sleep_us_cap: %d µs\n", max_sleep_us_cap);
            continue;
        }

        // Parse mode line (try new format with total_samples first, fall back to old format)
        if (sscanf(line, "%d %d %d %u", &mode, &num_pcs, &max_repeats, &total_samples) == 4) {
            if (mode < LEARNING_PROM_IDLE || mode > LEARNING_BUSY) continue;

            PCCollection *coll = &collections[mode - 1];
            coll->num_pcs = 0;
            coll->max_consecutive_repeats = max_repeats;
            coll->total_samples = total_samples;  // Restore total_samples!

            for (int i = 0; i < num_pcs && i < MAX_PC_CANDIDATES; i++) {
                unsigned long pc, npc;
                unsigned int count;
                if (fscanf(f, "0x%lx 0x%lx %u\n", &pc, &npc, &count) == 3) {
                    coll->pcs[i].pc = pc;
                    coll->pcs[i].npc = npc;
                    coll->pcs[i].count = count;
                    coll->num_pcs++;
                }
            }

            DEBUG_PRINTF("   Loaded %d %s PCs (samples=%u, max_repeats=%d)\n",
                        coll->num_pcs, coll->name, total_samples, max_repeats);
        } else if (sscanf(line, "%d %d %d", &mode, &num_pcs, &max_repeats) == 3) {
            // Old format without total_samples - use sum of counts as estimate
            if (mode < LEARNING_PROM_IDLE || mode > LEARNING_OS_IDLE) continue;

            PCCollection *coll = &collections[mode - 1];
            coll->num_pcs = 0;
            coll->max_consecutive_repeats = max_repeats;
            coll->total_samples = 0;  // Will calculate below

            for (int i = 0; i < num_pcs && i < MAX_PC_CANDIDATES; i++) {
                unsigned long pc, npc;
                unsigned int count;
                if (fscanf(f, "0x%lx 0x%lx %u\n", &pc, &npc, &count) == 3) {
                    coll->pcs[i].pc = pc;
                    coll->pcs[i].npc = npc;
                    coll->pcs[i].count = count;
                    coll->total_samples += count;  // Sum counts as estimate
                    coll->num_pcs++;
                }
            }
            DEBUG_PRINTF("   Loaded %d %s PCs (estimated samples=%u, max_repeats=%d) [OLD FORMAT]\n",
                        coll->num_pcs, coll->name, coll->total_samples, max_repeats);
        }
    }

    fclose(f);

    // Recalculate effective counts using Bayesian adjustment
    recalculate_effective_counts();

    fflush(stdout);
}

static void cpu_idle_start_learning_internal(LearningMode mode)
{
    if (mode <= LEARNING_OFF) return;

    int idx = mode - 1;  // Array index
    DEBUG_PRINTF("🎓 Starting %s PC learning mode...\n", collections[idx].name);

    // Reset collection BEFORE setting mode to prevent race condition
    collections[idx].num_pcs = 0;
    collections[idx].total_samples = 0;
    collections[idx].max_consecutive_repeats = 0;
    memset(collections[idx].pcs, 0, sizeof(collections[idx].pcs));

    // Now set learning mode (after reset to avoid counting during reset)
    learning_mode = mode;

    if (mode == LEARNING_BUSY) {
        DEBUG_PRINTF("   Keep guest BUSY (compile, run tasks) for 10 seconds\n");
    } else {
        DEBUG_PRINTF("   Keep guest IDLE for 10 seconds\n");
    }
    fflush(stdout);
}

static void cpu_idle_stop_learning_internal(void)
{
    if (learning_mode == LEARNING_OFF) {
        DEBUG_PRINTF("⚠️  No learning mode active\n");
        return;
    }

    LearningMode stopped_mode = learning_mode;
    PCCollection *coll = &collections[stopped_mode - 1];

    learning_mode = LEARNING_OFF;

    bool auto_stopped = (coll->total_samples >= LEARNING_AUTO_STOP_SAMPLES);
    DEBUG_PRINTF("🎓 %s PC learning complete%s!\n", coll->name,
                auto_stopped ? " (auto-stopped)" : "");
    DEBUG_PRINTF("   Total samples: %u\n", coll->total_samples);
    DEBUG_PRINTF("   Unique PC/NPC pairs: %d\n", coll->num_pcs);
    DEBUG_PRINTF("   Max consecutive repeats: %d\n", coll->max_consecutive_repeats);

    if (coll->num_pcs == 0) {
        DEBUG_PRINTF("   ⚠️  No PCs collected!\n");
        fflush(stdout);
        return;
    }

    // Sort by count (descending)
    for (int i = 0; i < coll->num_pcs - 1; i++) {
        for (int j = 0; j < coll->num_pcs - i - 1; j++) {
            if (coll->pcs[j].count < coll->pcs[j + 1].count) {
                PCCandidate temp = coll->pcs[j];
                coll->pcs[j] = coll->pcs[j + 1];
                coll->pcs[j + 1] = temp;
            }
        }
    }

    // Show top 10
    DEBUG_PRINTF("\n   Top PC/NPC pairs (by frequency):\n");
    DEBUG_PRINTF("   Rank  PC         NPC        Count     %%\n");
    DEBUG_PRINTF("   ----  --------   --------   -------   -----\n");
    int show_count = coll->num_pcs < 10 ? coll->num_pcs : 10;
    for (int i = 0; i < show_count; i++) {
        double percent = (double)coll->pcs[i].count / coll->total_samples * 100.0;
        DEBUG_PRINTF("   %2d.   0x%08x 0x%08x %7u   %5.1f%%\n", i + 1,
               (uint32_t)coll->pcs[i].pc, (uint32_t)coll->pcs[i].npc,
               coll->pcs[i].count, percent);
    }

    // Recalculate effective counts if we have busy collection and either PROM or OS idle collection.
    PCCollection *busy_coll = &collections[LEARNING_BUSY - 1];
    PCCollection *prom_coll = &collections[LEARNING_PROM_IDLE - 1];
    PCCollection *os_coll = &collections[LEARNING_OS_IDLE - 1];

    if (busy_coll->num_pcs > 0 &&
        (prom_coll->num_pcs > 0 || os_coll->num_pcs > 0)) {
        DEBUG_PRINTF("\n   🎯 Recalculating effective counts with BUSY and IDLE data...\n");
        recalculate_effective_counts();
    }

    // If this was BUSY mode, show adjusted idle PC list
    // (Confidence adjustment NOW applied - report shows freshly calculated values)
    if (stopped_mode == LEARNING_BUSY) {
        DEBUG_PRINTF("\n   🔍 Confidence-adjusted idle PCs (comparing idle vs busy frequency):\n");

        // Check against both idle collections
        for (int idle_type = 0; idle_type < 2; idle_type++) {  // 0=PROM, 1=OS
            PCCollection *idle_coll = &collections[idle_type];
            if (idle_coll->num_pcs == 0) continue;

            DEBUG_PRINTF("\n   %s (top 20, sorted by effective %%):\n", idle_coll->name);
            DEBUG_PRINTF("   Rank  PC         NPC        Original  Effective  Adjustment\n");
            DEBUG_PRINTF("   ----  --------   --------   --------  ---------  ----------\n");

            int show_max = idle_coll->num_pcs < 20 ? idle_coll->num_pcs : 20;
            for (int i = 0; i < show_max; i++) {
                double orig_pct = (double)idle_coll->pcs[i].count / idle_coll->total_samples * 100.0;
                double eff_pct = (double)idle_coll->pcs[i].effective_count / idle_coll->total_samples * 100.0;

                DEBUG_PRINTF("   %2d.   0x%08x 0x%08x %7.1f%%  %8.1f%%", i + 1,
                       (uint32_t)idle_coll->pcs[i].pc,
                       (uint32_t)idle_coll->pcs[i].npc,
                       orig_pct, eff_pct);

                // Show adjustment if there was one
                if (idle_coll->pcs[i].effective_count != idle_coll->pcs[i].count) {
                    if (eff_pct < orig_pct) {
                        // Reduced confidence (more in busy than idle)
                        double reduction_pct = ((orig_pct - eff_pct) / orig_pct) * 100.0;
                        DEBUG_PRINTF("  ↓%.0f%%", reduction_pct);
                    } else {
                        // Increased confidence (more in idle than busy)
                        double boost_pct = ((eff_pct - orig_pct) / orig_pct) * 100.0;
                        DEBUG_PRINTF("  ↑%.0f%%", boost_pct);
                    }
                }
                DEBUG_PRINTF("\n");
            }
        }
    }

    // Always auto-save after learning (BUSY or otherwise)
    // Use first CPU to get arch name (all CPUs in a session are same architecture)
    CPUState *first_cpu_state = first_cpu;
    if (first_cpu_state) {
        const char *arch_name = get_arch_name_from_cpu(first_cpu_state);
        save_learned_pcs(arch_name);
    }

    fflush(stdout);
}

// CPU execution enter hook - called before EVERY Translation Block execution
void cpu_idle_exec_hook(CPUState *cs)
{
    // If both idle detection AND debug are off, exit early
    if (!cpu_idle_enabled && !cpu_idle_debug) {
        return;
    }

    // Measure hook entry time for overhead calculation
    int64_t hook_entry_time_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    // Auto-initialize on first call
    static bool initialized = false;
    if (!initialized) {
        cpu_idle_init();
        initialized = true;
    }

    // Extract architecture name from CPU class
    const char *arch_name = get_arch_name_from_cpu(cs);
    CPUPCState pc_state;
    cpu_get_pc_state(cs, &pc_state);
    static uint64_t last_pc = 0;
    static uint64_t last_npc = 0;
    static int total_sleep_us = 0;  // Track total sleep time per second
    static int min_sleep_us = INT_MAX;  // Track minimum sleep per second
    static int max_sleep_us = 0;  // Track maximum sleep per second
    static int sleep_events = 0;     // Count of actual sleep calls per second
    static int total_execs = 0;      // Total exec_enter calls per second (for percentage calculation)
    static int prev_total_execs = 1;  // Previous second's total_execs (for PROM gating calculation)
    static int prev_prom_total_hits = 0; // Previous second's total PROM hits
    static int os_idle_hits = 0;  // Count OS idle detections per second
    static int prom_idle_hits = 0;   // Count PROM idle detections per second
    static int generic_idle_hits = 0; // Count generic idle loop detections per second
    static int antigeneric_hits = 0;  // Count generic repeats rejected by BUSY collection
    static double antigeneric_freq_sum = 0.0; // Sum of BUSY freq % for antigeneric hits
    static int busy_hits = 0;         // Count normal busy execs (not idle, not generic)
    static int halted_hits = 0;       // Count cpu halted state per second
    static double freq_pct_sum = 0.0;  // Sum of PC frequency percentages (for weighted idle %)
    static double freq_pct_min = 100.0;  // Minimum PC frequency % this second
    static double freq_pct_max = 0.0;    // Maximum PC frequency % this second
    static int64_t total_hook_time_ns = 0;  // Total time spent in this hook per second

    // Known idle addresses (discovered through analysis)

    // Learning mode: collect PC/NPC frequencies (DRY approach)
    static int learning_consecutive_count = 0;
    static uint64_t learning_last_pc = 0;
    static uint64_t learning_last_npc = 0;

    if (learning_mode != LEARNING_OFF) {
        PCCollection *coll = &collections[learning_mode - 1];  // mode-1 since OFF has no collection
        coll->total_samples++;

        // Auto-stop at 10K samples
        if (coll->total_samples >= LEARNING_AUTO_STOP_SAMPLES) {
            cpu_idle_stop_learning_internal();
            // Note: learning_mode is now OFF, will continue to idle detection below
        } else {
            // Track consecutive repeats for threshold calculation
            if (pc_state.pc == learning_last_pc && pc_state.next_pc == learning_last_npc) {
                learning_consecutive_count++;
                if (learning_consecutive_count > coll->max_consecutive_repeats) {
                    coll->max_consecutive_repeats = learning_consecutive_count;
                }
            } else {
                learning_consecutive_count = 1;
                learning_last_pc = pc_state.pc;
                learning_last_npc = pc_state.next_pc;
            }

            // Find or add this PC/NPC pair
            int found = -1;
            for (int i = 0; i < coll->num_pcs; i++) {
                if (coll->pcs[i].pc == pc_state.pc && coll->pcs[i].npc == pc_state.next_pc) {
                    found = i;
                    break;
                }
            }

            if (found >= 0) {
                coll->pcs[found].count++;
            } else if (coll->num_pcs < MAX_PC_CANDIDATES) {
                coll->pcs[coll->num_pcs].pc = pc_state.pc;
                coll->pcs[coll->num_pcs].npc = pc_state.next_pc;
                coll->pcs[coll->num_pcs].count = 1;
                coll->num_pcs++;
            }
        }
    }

    // Check for learned idle patterns and calculate sleep based on frequency
    bool is_known_idle = false;
    int sleep_us = 0;
    uint32_t pc_frequency = 0;  // How often this PC appeared during learning
    total_execs++;  // Count every execution for percentage calculation

    // Check both idle collections (PROM and OS) - use learned frequency!
    for (int idle_type = 0; idle_type < 2 && !is_known_idle; idle_type++) {
        PCCollection *idle_coll = &collections[idle_type];  // 0=PROM, 1=OS

        if (idle_coll->num_pcs > 0) {
            // Use learned PCs WITH frequency data
            for (int i = 0; i < idle_coll->num_pcs; i++) {
                if (pc_state.pc == idle_coll->pcs[i].pc && pc_state.next_pc == idle_coll->pcs[i].npc) {
                    is_known_idle = true;
                    pc_frequency = idle_coll->pcs[i].effective_count;

                    // Calculate sleep based on frequency: linear mapping, capped at current_sleep_cap
                    if (idle_coll->total_samples > 0) {
                        double freq_pct = (double)pc_frequency / idle_coll->total_samples * 100.0;

                        // Clamp freq_pct to 100% max (effective_count can be UINT32_MAX for perfect idle indicators)
                        if (freq_pct > 100.0) freq_pct = 100.0;

                        freq_pct_sum += freq_pct;  // Accumulate for weighted idle % calculation
                        if (freq_pct < freq_pct_min) freq_pct_min = freq_pct;
                        if (freq_pct > freq_pct_max) freq_pct_max = freq_pct;
                        sleep_us = (int)(freq_pct * 1000);  // Direct linear: 1%=10µs, 10%=100µs, 50%=500µs, 100%=1000µs
                        if (sleep_us > current_sleep_cap) {
                            sleep_us = current_sleep_cap;  // Cap at dynamically-adjusted limit
                        }
                    }

                    // Count idle type for reporting
                    if (idle_type == 1) {  // OS - always allow sleeping
                        os_idle_hits++;
                    } else {  // PROM - only sleep if >90% of execs were PROM idle in previous second
                        prom_idle_hits++;
                        // Use PREVIOUS second's data to avoid early-second false readings
                        double prev_prom_pct = (double)prev_prom_total_hits / prev_total_execs * 100.0;
                        if (prev_prom_pct < 90.0) {
                            sleep_us = 0;  // Don't sleep - we're not truly PROM-idling
                        }
                    }
                    break;
                }
            }
        }
    }

    // Generic idle detection (PC/NPC repeat) - only if not already known-idle
    if (!is_known_idle && pc_state.pc == last_pc && pc_state.next_pc == last_npc) {
        // Check if this PC/NPC is in the BUSY collection (filter out busy loops!)
        bool is_busy_pc = false;
        PCCollection *busy_coll = &collections[LEARNING_BUSY - 1];
        for (int i = 0; i < busy_coll->num_pcs; i++) {
            if (busy_coll->pcs[i].pc == pc_state.pc && busy_coll->pcs[i].npc == pc_state.next_pc) {
                is_busy_pc = true;
                // Track the busy frequency for reporting (antigeneric = rejected generic idle)
                if (busy_coll->total_samples > 0) {
                    double busy_freq_pct = (double)busy_coll->pcs[i].count / busy_coll->total_samples * 100.0;
                    antigeneric_freq_sum += busy_freq_pct;
                    antigeneric_hits++;
                }
                break;
            }
        }

        // Only treat as idle if NOT in busy collection
        if (!is_busy_pc) {
            generic_idle_hits++;
            freq_pct_sum += 1.0;  // Low confidence (generic repeat)
            if (1.0 < freq_pct_min) freq_pct_min = 1.0;
            if (1.0 > freq_pct_max) freq_pct_max = 1.0;
            // Generic sleep: just a small delay (not trusted like learned PCs)
            sleep_us = 1000;  // 1ms for generic repeats
        }
    } else if (!is_known_idle && !cs->halted) {
        // Normal busy execution (PC changed, not idle, not halted)
        busy_hits++;
    }

    // Check for power down state
    if (cs->halted) {
        sleep_us = current_sleep_cap;
        freq_pct_sum += 100.0;  // Halted = 100% idle
        if (100.0 > freq_pct_max) freq_pct_max = 100.0;
        halted_hits++;
    }

    // Track sleep statistics (including zeros)
    total_sleep_us += sleep_us;
    if (sleep_us > 0) {
        sleep_events++;
        if (sleep_us > max_sleep_us) max_sleep_us = sleep_us;
    }
    if (sleep_us < min_sleep_us) min_sleep_us = sleep_us;

    // Only sleep if idle detection is enabled AND not in learning mode
    // (need unthrottled speed for accurate learning)
    if (cpu_idle_enabled && learning_mode == LEARNING_OFF && sleep_us > 0) {
        g_usleep(sleep_us);
    }

    // Print debug info at most once per second (MAME-style speed measurement)
    static int64_t last_report_time_ns = 0;
    static int64_t last_vm_clock_ns = 0;

    int64_t current_time_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);  // Host wall-clock
    int64_t vm_clock_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);       // Guest virtual time

    if (last_report_time_ns == 0) {
        last_report_time_ns = current_time_ns;
    }

    if (total_execs > 0 && current_time_ns - last_report_time_ns >= 1000000000) {  // Every second
        // Check if config file was updated and reload if needed
        struct stat st;
        if (stat_with_tmp_fallback(get_idle_pc_save_file(arch_name), &st) == 0) {
            if (st.st_mtime != last_config_mtime) {
                if (last_config_mtime > 0) {  // Not first time
                    DEBUG_PRINTF("📂 Config file updated, reloading...\n");
                    load_learned_pcs(arch_name);
                }
                last_config_mtime = st.st_mtime;
            }
        }

        int total_idle = os_idle_hits + prom_idle_hits + generic_idle_hits + halted_hits;
        int64_t real_delta_ns = current_time_ns - last_report_time_ns;
        int64_t vm_delta_ns = vm_clock_ns - last_vm_clock_ns;

        // MAME-style speed calculation: (emulated_time / real_time) * 100
        int speed_percent = (int)((double)vm_delta_ns / (double)real_delta_ns * 100.0);

        // Get vCPU thread wait time to detect emulator capacity
        int64_t vcpu_wait_time_ns = qemu_get_vcpu_wait_time_ns();
        int vcpu_wait_ms = (int)(vcpu_wait_time_ns / 1000000);
        int vcpu_wait_pct = real_delta_ns > 0 ? (int)((double)vcpu_wait_time_ns / (double)real_delta_ns * 100.0) : 0;

        qemu_reset_vcpu_wait_stats();  // Reset for next second

        // Auto-tune sleep cap to maintain ~100% speed (works WITH icount, not instead of it!)
        // - icount adjusts virtual time (ns/instruction) to keep clocks in sync
        // - auto-tune adjusts sleep duration to maintain execution speed at ~100%
        //
        // Smart coordination with icount auto mode:
        // - If icount is in auto mode AND host has capacity AND icount is NOT maxed out → let icount handle it
        // - If icount is maxed out OR host is maxed out OR icount is fixed → auto-tune helps
        int old_sleep_cap = current_sleep_cap;
        bool icount_auto_mode = (icount_enabled() == ICOUNT_ADAPTATIVE);
        bool icount_maxed = icount_enabled() && (icount_get_shift() >= MAX_ICOUNT_SHIFT);
        bool host_has_capacity = (vcpu_wait_pct > 50);
        bool icount_can_help = icount_auto_mode && host_has_capacity && !icount_maxed;
        bool should_auto_tune = auto_tune_sleep && !icount_can_help;

        if (should_auto_tune) {
            // If guest is falling behind real time, reduce sleep to let it catch up
            // If guest is ahead, increase sleep to slow it down
            // Target: speed_percent close to 100%

            if (speed_percent < 95) {
                // Guest is slow - reduce sleep by 10%
                current_sleep_cap = (current_sleep_cap * 90) / 100;
                if (current_sleep_cap < 100) current_sleep_cap = 100;  // Min 100µs
            } else if (speed_percent > 105) {
                // Guest is fast - increase sleep by 10%
                current_sleep_cap = (current_sleep_cap * 110) / 100;
                if (current_sleep_cap > max_sleep_us_cap) current_sleep_cap = max_sleep_us_cap;
            }
        }

        // Calculate VM clock drift rate (how much VM gained/lost vs real time this second)
        // Positive = VM running faster than real time, Negative = VM running slower
        int64_t drift_rate_ns = vm_delta_ns - real_delta_ns;

        // Track cumulative drift over time
        static int64_t cumulative_drift_ns = 0;
        cumulative_drift_ns += drift_rate_ns;

        const char *drift_direction = "";
        char drift_str[64] = "";

        drift_direction = cumulative_drift_ns > 0 ? "+" : "-";
        char drift_abs_str[32];
        format_time_delta(cumulative_drift_ns < 0 ? -cumulative_drift_ns : cumulative_drift_ns, drift_abs_str, sizeof(drift_abs_str));
        snprintf(drift_str, sizeof(drift_str), " vm:%s%s", drift_direction, drift_abs_str);

        // Calculate true idle percentage (weighted by PC frequency from learning)
        double idle_pct = total_idle > 0 ? freq_pct_sum / total_idle : 0.0;

        // Calculate average sleep PER EXEC (not just per sleep event)
        // This includes all the execs where sleep_us = 0
        int avg_sleep_us = total_execs > 0 ? total_sleep_us / total_execs : 0;

        // Build complete message to avoid interleaving (thread-safe)
        char msg[512];
        int pos;

        // Show learning progress if in learning mode
        if (learning_mode != LEARNING_OFF) {
            PCCollection *learn_coll = &collections[learning_mode - 1];
            double learn_pct = (double)learn_coll->total_samples / LEARNING_AUTO_STOP_SAMPLES * 100.0;
            pos = snprintf(msg, sizeof(msg), "[LEARNING %s] %d/%d samples (%.1f%%) spd=%d%% execs=%d",
                          learn_coll->name, learn_coll->total_samples, LEARNING_AUTO_STOP_SAMPLES,
                          learn_pct, speed_percent, total_execs);
        } else {
            int hook_time_ms = (int)(total_hook_time_ns / 1000000);
            int hook_pct = real_delta_ns > 0 ? (int)((double)total_hook_time_ns / (double)real_delta_ns * 100.0) : 0;

            pos = snprintf(msg, sizeof(msg), "[CPU-IDLE] spd=%d%%%s exec=%d idle=%.1f%%(%.1f-%.1f%%) sleep:%d/%d/%dµs(%devt) hook:%dms(%d%%) vcpu-wait:%dms(%d%%)",
                          speed_percent, drift_str, total_execs, idle_pct,
                          freq_pct_min == 100.0 ? 0.0 : freq_pct_min,
                          freq_pct_max,
                          min_sleep_us == INT_MAX ? 0 : min_sleep_us,
                          avg_sleep_us,
                          max_sleep_us,
                          sleep_events,
                          hook_time_ms,
                          hook_pct,
                          vcpu_wait_ms,
                          vcpu_wait_pct);

            // Warning if vCPU wait time is very low (approaching capacity limit) - disabled until we find reliable way to detect this
            /*if (vcpu_wait_pct < 10 && icount_enabled()) {
                pos += snprintf(msg + pos, sizeof(msg) - pos, " ⚠️ VCPU MAXED OUT!");
            }*/

            // Show sleep cap adjustment if it changed
            if (auto_tune_sleep && old_sleep_cap != current_sleep_cap) {
                pos += snprintf(msg + pos, sizeof(msg) - pos, " sleepcap:%d→%dµs",
                               old_sleep_cap, current_sleep_cap);
            }
        }

        // Show breakdown if there are idle hits
        if (total_idle > 0) {
            pos += snprintf(msg + pos, sizeof(msg) - pos, " [");
            bool first = true;
            if (os_idle_hits > 0) {
                double os_pct = (double)os_idle_hits / total_execs * 100.0;
                pos += snprintf(msg + pos, sizeof(msg) - pos, "%sO:%.1f%%", first ? "" : " ", os_pct);
                first = false;
            }
            if (prom_idle_hits > 0) {
                double prom_pct = (double)prom_idle_hits / total_execs * 100.0;
                pos += snprintf(msg + pos, sizeof(msg) - pos, "%sP:%.1f%%", first ? "" : " ", prom_pct);
                first = false;
            }
            if (generic_idle_hits > 0) {
                double generic_pct = (double)generic_idle_hits / total_execs * 100.0;
                pos += snprintf(msg + pos, sizeof(msg) - pos, "%sG:%.1f%%", first ? "" : " ", generic_pct);
                first = false;
            }
            if (antigeneric_hits > 0) {
                double antigen_pct = (double)antigeneric_hits / total_execs * 100.0;
                double avg_antigen_freq = antigeneric_freq_sum / antigeneric_hits;
                pos += snprintf(msg + pos, sizeof(msg) - pos, "%sAG:%.1f%%(freq:%.1f%%)",
                               first ? "" : " ", antigen_pct, avg_antigen_freq);
                first = false;
            }
            if (halted_hits > 0) {
                double halted_pct = (double)halted_hits / total_execs * 100.0;
                pos += snprintf(msg + pos, sizeof(msg) - pos, "%sH:%.1f%%", first ? "" : " ", halted_pct);
                first = false;
            }
            if (busy_hits > 0) {
                double busy_pct = (double)busy_hits / total_execs * 100.0;
                pos += snprintf(msg + pos, sizeof(msg) - pos, "%sB:%.1f%%",
                               first ? "" : " ", busy_pct);
            }
            pos += snprintf(msg + pos, sizeof(msg) - pos, "]");
        }

        snprintf(msg + pos, sizeof(msg) - pos, "\n");
        DEBUG_PRINTF("%s", msg);

        last_report_time_ns = current_time_ns;
        last_vm_clock_ns = vm_clock_ns;
        total_sleep_us = 0;
        min_sleep_us = INT_MAX;
        max_sleep_us = 0;
        sleep_events = 0;
        // Save current values as "previous" for next second's PROM gating check
        prev_total_execs = total_execs > 0 ? total_execs : 1;  // Avoid div by zero
        prev_prom_total_hits = prom_idle_hits;
        os_idle_hits = prom_idle_hits = 0;
        generic_idle_hits = halted_hits = 0;
        antigeneric_hits = 0;
        antigeneric_freq_sum = 0.0;
        busy_hits = 0;
        total_execs = 0;  // Reset for next second
        freq_pct_sum = 0.0;  // Reset weighted idle % accumulator
        freq_pct_min = 100.0;  // Reset min/max for next second
        freq_pct_max = 0.0;
        total_hook_time_ns = 0;  // Reset hook overhead timer
    }

    last_pc = pc_state.pc; last_npc = pc_state.next_pc;

    // Measure hook exit time and accumulate total (must be last thing in function!)
    int64_t hook_exit_time_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    total_hook_time_ns += (hook_exit_time_ns - hook_entry_time_ns);
}

// Functions to control idle detection and debug output
void cpu_idle_set_enabled(bool enable)
{
    cpu_idle_enabled = enable;
    fprintf(stderr, "CPU idle detection %s\n", enable ? "enabled" : "disabled");
}

void cpu_idle_set_debug(bool enable)
{
    cpu_idle_debug = enable;
    DEBUG_PRINTF("CPU idle debug output %s\n", enable ? "enabled" : "disabled");
}

void hmp_cpu_idle(Monitor *mon, const QDict *qdict)
{
    bool enable = qdict_get_bool(qdict, "enable");
    cpu_idle_set_enabled(enable);
    monitor_printf(mon, "CPU idle detection %s\n", enable ? "enabled" : "disabled");
}

void hmp_cpu_idle_debug(Monitor *mon, const QDict *qdict)
{
    bool enable = qdict_get_bool(qdict, "enable");
    cpu_idle_set_debug(enable);
    monitor_printf(mon, "CPU idle debug output %s\n", enable ? "enabled" : "disabled");
}

void hmp_cpu_idle_start_prom_learning(Monitor *mon, const QDict *qdict)
{
    cpu_idle_start_learning_internal(LEARNING_PROM_IDLE);
    monitor_printf(mon, "Started PROM idle PC learning mode\n");
}

void hmp_cpu_idle_start_os_learning(Monitor *mon, const QDict *qdict)
{
    cpu_idle_start_learning_internal(LEARNING_OS_IDLE);
    monitor_printf(mon, "Started OS idle PC learning mode\n");
}

void hmp_cpu_idle_start_busy_learning(Monitor *mon, const QDict *qdict)
{
    cpu_idle_start_learning_internal(LEARNING_BUSY);
    monitor_printf(mon, "Started busy PC learning mode\n");
}

void hmp_cpu_idle_stop_learning(Monitor *mon, const QDict *qdict)
{
    cpu_idle_stop_learning_internal();
    monitor_printf(mon, "Stopped learning mode\n");
}

void hmp_info_icount(Monitor *mon, const QDict *qdict)
{
    ICountMode mode = icount_enabled();

    if (mode == ICOUNT_DISABLED) {
        monitor_printf(mon, "icount: disabled\n");
        return;
    }

    int shift = icount_get_shift();
    const char *mode_str = (mode == ICOUNT_PRECISE) ? "precise (fixed)" : "adaptive (auto)";

    monitor_printf(mon, "icount: %s\n", mode_str);
    monitor_printf(mon, "  shift: %d (2^%d = %d ns/instruction)\n",
                  shift, shift, 1 << shift);
}

void hmp_icount_shift_set(Monitor *mon, const QDict *qdict)
{
    const char *shift_str = qdict_get_str(qdict, "shift");

    if (!icount_enabled()) {
        monitor_printf(mon, "Error: icount not enabled (use -icount at startup)\n");
        return;
    }

    // Check if the user wants auto mode
    if (strcmp(shift_str, "auto") == 0) {
        // Re-enable auto-adjust mode
        icount_enable_adaptive();
        monitor_printf(mon, "icount: auto-adjust mode enabled (will dynamically tune shift)\n");
        return;
    }

    // Parse as integer
    char *endptr;
    long shift = strtol(shift_str, &endptr, 10);

    if (*endptr != '\0' || shift < 0 || shift > MAX_ICOUNT_SHIFT) {
        monitor_printf(mon, "Error: shift must be 'auto' or 0-%d\n", MAX_ICOUNT_SHIFT);
        return;
    }

    // Set fixed shift - switch to precise mode (disables auto-adjust)
    icount_enable_precise();
    icount_set_shift(shift);
    monitor_printf(mon, "icount shift set to %ld (2^%ld = %d ns/instruction, auto-adjust disabled)\n",
                  shift, shift, 1 << (int)shift);
}

// ============================================================================
// Public API Wrappers
// ============================================================================

void cpu_idle_start_prom_learning(void)
{
    cpu_idle_start_learning_internal(LEARNING_PROM_IDLE);
}

void cpu_idle_start_os_idle_learning(void)
{
    cpu_idle_start_learning_internal(LEARNING_OS_IDLE);
}

void cpu_idle_start_busy_learning(void)
{
    cpu_idle_start_learning_internal(LEARNING_BUSY);
}

void cpu_idle_stop_learning(void)
{
    cpu_idle_stop_learning_internal();
}

void cpu_idle_init(void)
{
    // Load learned PCs from disk (called once during CPU initialization)
    // Get architecture name from first CPU
    static bool initialized = false;
    if (!initialized && first_cpu) {
        const char *arch_name = get_arch_name_from_cpu(first_cpu);
        load_learned_pcs(arch_name);
        initialized = true;
    }
}
