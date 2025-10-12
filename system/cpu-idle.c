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
#include "hw/core/cpu.h"
#include "system/cpu-idle.h"
#include "system/cpu-timers.h"
#include "system/cpu-throttle.h"
#include "monitor/monitor.h"
#include <sys/stat.h>

/* Debug output helper - use stderr to not interfere with serial console */
#define DEBUG_PRINTF(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while(0)

/* Global debug flag */
static bool cpu_idle_debug = false;

/* PC learning system for idle detection */
typedef enum {
    LEARNING_OFF = 0,
    LEARNING_PROM_IDLE,
    LEARNING_OS_IDLE,      // Renamed from SUNOS_IDLE to be generic
    LEARNING_BUSY
} LearningMode;

#define NUM_COLLECTIONS 3  // PROM_IDLE, OS_IDLE, BUSY

static LearningMode learning_mode = LEARNING_OFF;

#define MAX_PC_CANDIDATES 1000
#define LEARNING_AUTO_STOP_SAMPLES 10000  // Auto-stop learning after 10K samples

/* PC candidate structure */
typedef struct {
    target_ulong pc;
    target_ulong npc;
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
static time_t last_config_mtime = 0;   // Track config file mtime for auto-reload

/* Forward declarations */
static void cpu_idle_stop_learning_internal(void);
static void load_learned_pcs(const char *arch_name);
static void save_learned_pcs(const char *arch_name);
static int stat_with_tmp_fallback(const char *filename, struct stat *st);
static void recalculate_effective_counts(void);

/* Hardcoded fallback idle PCs (architecture-specific, loaded from target code) */
static target_ulong *fallback_prom_pcs = NULL;
static int fallback_prom_pc_count = 0;
static target_ulong fallback_os_idle_pc = 0;

/**
 * cpu_idle_register_fallback_pcs - Register architecture's hardcoded idle PCs
 *
 * Called by architecture-specific code to provide fallback PCs when no
 * learned data is available.
 */
void cpu_idle_register_fallback_pcs(target_ulong *prom_pcs, int prom_count,
                                     target_ulong os_idle_pc)
{
    fallback_prom_pcs = prom_pcs;
    fallback_prom_pc_count = prom_count;
    fallback_os_idle_pc = os_idle_pc;
}

/* Helper: Get save filename based on architecture and icount mode */
static const char *get_idle_pc_save_file(const char *arch_name, bool for_stat)
{
    static char filename[256];
    const char *suffix = icount_enabled() ? "-icount" : "";

    if (for_stat) {
        /* Try current directory first, then /tmp */
        snprintf(filename, sizeof(filename), "qemu-%s-idle-pcs%s.dat",
                arch_name, suffix);
    } else {
        snprintf(filename, sizeof(filename), "qemu-%s-idle-pcs%s.dat",
                arch_name, suffix);
    }

    return filename;
}

/* Helper: fopen with /tmp fallback */
static FILE *fopen_with_tmp_fallback(const char *filename, const char *mode)
{
    FILE *f = fopen(filename, mode);
    if (!f) {
        char tmp_path[256];
        snprintf(tmp_path, sizeof(tmp_path), "/tmp/%s", filename);
        f = fopen(tmp_path, mode);
    }
    return f;
}

/* Helper: stat with /tmp fallback */
static int stat_with_tmp_fallback(const char *filename, struct stat *st)
{
    if (stat(filename, st) == 0) {
        return 0;  // Success in CWD
    }
    // Try /tmp
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/%s", filename);
    return stat(tmp_path, st);
}

/* TODO: Add remaining functions from target/sparc/cpu.c:
 * - recalculate_effective_counts()
 * - compare_pc_candidates()
 * - save_learned_pcs()
 * - load_learned_pcs()
 * - cpu_idle_exec_hook() - main detection logic
 * - cpu_idle_start_learning()
 * - cpu_idle_stop_learning()
 * - HMP command handlers
 */

/* Stub implementations for now */
void cpu_idle_exec_hook(CPUState *cpu)
{
    /* TODO: Move from sparc_cpu_exec_enter_hook() */
}

void cpu_idle_set_debug(bool enable)
{
    cpu_idle_debug = enable;
    DEBUG_PRINTF("CPU idle detection debug logging %s\n", enable ? "enabled" : "disabled");
}

void cpu_idle_start_prom_learning(void)
{
    /* TODO: Implement */
}

void cpu_idle_start_os_idle_learning(void)
{
    /* TODO: Implement */
}

void cpu_idle_start_busy_learning(void)
{
    /* TODO: Implement */
}

void cpu_idle_stop_learning(void)
{
    /* TODO: Implement */
}

void cpu_idle_init(void)
{
    /* TODO: Load learned PCs on startup */
}

