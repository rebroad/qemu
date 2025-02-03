/*
 * Sparc CPU init helpers
 *
 *  Copyright (c) 2003-2005 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <fcntl.h>
#include <termios.h>
#include <math.h>
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "cpu.h"
#include "qemu/module.h"
#include "qemu/qemu-print.h"
#include "exec/exec-all.h"
#include "exec/translation-block.h"
#include "hw/qdev-properties.h"
#include "qapi/visitor.h"
#include "tcg/tcg.h"
#include "fpu/softfloat.h"
#include "target/sparc/translate.h"

//#define DEBUG_FEATURES

static void sparc_cpu_reset_hold(Object *obj, ResetType type)
{
    CPUState *cs = CPU(obj);
    SPARCCPUClass *scc = SPARC_CPU_GET_CLASS(obj);
    CPUSPARCState *env = cpu_env(cs);

    if (scc->parent_phases.hold) {
        scc->parent_phases.hold(obj, type);
    }

    memset(env, 0, offsetof(CPUSPARCState, end_reset_fields));
    env->cwp = 0;
#ifndef TARGET_SPARC64
    env->wim = 1;
#endif
    env->regwptr = env->regbase + (env->cwp * 16);
#if defined(CONFIG_USER_ONLY)
#ifdef TARGET_SPARC64
    env->cleanwin = env->nwindows - 2;
    env->cansave = env->nwindows - 2;
    env->pstate = PS_RMO | PS_PEF | PS_IE;
    env->asi = 0x82; /* Primary no-fault */
#endif
#else
#if !defined(TARGET_SPARC64)
    env->psret = 0;
    env->psrs = 1;
    env->psrps = 1;
#endif
#ifdef TARGET_SPARC64
    env->pstate = PS_PRIV | PS_RED | PS_PEF;
    if (!cpu_has_hypervisor(env)) {
        env->pstate |= PS_AG;
    }
    env->hpstate = cpu_has_hypervisor(env) ? HS_PRIV : 0;
    env->tl = env->maxtl;
    env->gl = 2;
    cpu_tsptr(env)->tt = TT_POWER_ON_RESET;
    env->lsu = 0;
#else
    env->mmuregs[0] &= ~(MMU_E | MMU_NF);
    env->mmuregs[0] |= env->def.mmu_bm;
#endif
    env->pc = 0;
    env->npc = env->pc + 4;
#endif
    env->cache_control = 0;
    cpu_put_fsr(env, 0);
}

#ifndef CONFIG_USER_ONLY
static long long timespec_diff_ns(struct timespec *start, struct timespec *end) {
    return (end->tv_sec - start->tv_sec) * 1000000000LL +
           (end->tv_nsec - start->tv_nsec);
}

static void timespecadd(struct timespec *a, const struct timespec *b)
{
    a->tv_sec += b->tv_sec;
    a->tv_nsec += b->tv_nsec;

    // Normalize to ensure tv_nsec is between 0 and 999,999,999
    while (a->tv_nsec >= 1000000000) {
        a->tv_sec++;
        a->tv_nsec -= 1000000000;
    }
}

struct SleepCriteria {
    bool on_true;           // Whether to sleep on TRUE results
    bool compare_over;      // true = OVER, false = BELOW
    long long threshold;    // Threshold in nanoseconds
    bool valid;            // Whether criteria was successfully parsed
};

// Add this function to parse the sleep file
static struct SleepCriteria parse_sleep_file(const char *filename) {
    struct SleepCriteria criteria = {0};
    FILE *file = fopen(filename, "r");
    if (!file) {
        return criteria;
    }

    char line[256];
    char value[32];
    
    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, "RESULT=%s", value) == 1) {
            criteria.on_true = (strcmp(value, "TRUE") == 0);
        }
        else if (sscanf(line, "COMPARE=%s", value) == 1) {
            criteria.compare_over = (strcmp(value, "OVER") == 0);
        }
        else if (sscanf(line, "THRESHOLD=%lld", &criteria.threshold) == 1) {
            criteria.valid = true;
        }
    }
    
    fclose(file);
    return criteria;
}

static bool should_sleep(struct SleepCriteria criteria, bool result, long long interval) {
    if (!criteria.valid) {
        return false;
    }
    
    // Only proceed if we're checking for the correct result type
    if (criteria.on_true != result) {
        return false;
    }
    
    // Check if interval meets the threshold criteria
    if (criteria.compare_over) {
        return interval > criteria.threshold;
    } else {
        return interval < criteria.threshold;
    }
}

#define NUM_BANDS 256
#define HISTORY_SECONDS 60

struct TimingSpectrum {
    // Use logarithmic bands to better capture the 300ns - 100ms range
    unsigned long long band_boundaries[NUM_BANDS + 1];
    unsigned int counts[NUM_BANDS];
    unsigned int total_samples;
    time_t timestamp;
};

struct SystemState {
    struct TimingSpectrum false_intervals[HISTORY_SECONDS];
    struct TimingSpectrum true_intervals[HISTORY_SECONDS];
    struct TimingSpectrum total_intervals[HISTORY_SECONDS];
    int current_vm_state;  // 0 for idle, 1 for busy
    int current_index;     // Current position in circular buffer
};

static void initialize_bands(unsigned long long *boundaries) {
    // Min timing: ~300ns, Max timing: ~100ms
    double min_log = log10(300.0);
    double max_log = log10(100000000.0);
    double step = (max_log - min_log) / NUM_BANDS;

    for (int i = 0; i <= NUM_BANDS; i++) {
        boundaries[i] = (unsigned long long)pow(10, min_log + (step * i));
    }
}

static void add_timing_sample(struct TimingSpectrum *spectrum, unsigned long long timing) {
    for (int i = 0; i < NUM_BANDS; i++) {
        if (timing < spectrum->band_boundaries[i + 1]) {
            spectrum->counts[i]++;
            break;
        }
    }
    spectrum->total_samples++;
}

static void output_csv(FILE *f, const struct SystemState *state) {
    fprintf(f, "Timestamp,VMState,IntervalType,Band,LowerBound,UpperBound,Count\n");

    for (int t = 0; t < HISTORY_SECONDS; t++) {
        for (int b = 0; b < NUM_BANDS; b++) {
            fprintf(f, "%ld,%d,FALSE,%d,%llu,%llu,%u\n",
                   state->false_intervals[t].timestamp,
                   state->current_vm_state, b,
                   state->false_intervals[t].band_boundaries[b],
                   state->false_intervals[t].band_boundaries[b+1],
                   state->false_intervals[t].counts[b]);
            fprintf(f, "%ld,%d,TRUE,%d,%llu,%llu,%u\n",
                   state->true_intervals[t].timestamp,
                   state->current_vm_state, b,
                   state->true_intervals[t].band_boundaries[b],
                   state->true_intervals[t].band_boundaries[b+1],
                   state->true_intervals[t].counts[b]);
            fprintf(f, "%ld,%d,TOTAL,%d,%llu,%llu,%u\n",
                   state->total_intervals[t].timestamp,
                   state->current_vm_state, b,
                   state->total_intervals[t].band_boundaries[b],
                   state->total_intervals[t].band_boundaries[b+1],
                   state->total_intervals[t].counts[b]);
        }
    }
}

static bool sparc_cpu_exec_interrupt(CPUState *cs, int interrupt_request)
{
    bool result = false;
    if (interrupt_request & CPU_INTERRUPT_HARD) {
        CPUSPARCState *env = cpu_env(cs);
        if (cpu_interrupts_enabled(env) && env->interrupt_index > 0) {
            int pil = env->interrupt_index & 0xf;
            int type = env->interrupt_index & 0xf0;
            if (type != TT_EXTINT || cpu_pil_allowed(env, pil)) {
                cs->exception_index = env->interrupt_index;
                sparc_cpu_do_interrupt(cs);
                result = true;
            }
        }
    }

	static struct SystemState system_state = {0};
	static bool bands_initialized = false;
	static FILE *csv_file = NULL;
    static time_t last_print_time = 0, last_measure_time = 0, last_vm_state_check = 0;
    static bool sleep_enabled = true, measuring_mode = false;
    static int true_count = 0, last_true_count = 0, false_count = 0, sleeps = 0, natural_true_rate = 100;
    static struct timespec last_true_time, last_false_time;
    static long long true_interval_ns, min_true_interval, max_true_interval;
    static long long false_interval_ns, min_false_interval, max_false_interval;
	static long long last_min_false_interval = 0;
    static long to_sleep = 19000, erm_sleep = 0; // microseconds
    static int current_true_streak = 0, current_false_streak = 0;
    static int min_true_streak = 0, max_true_streak = 0, last_max_true_streak = 0;
    static int min_false_streak = 0, last_min_false_streak = 0;
    static int max_false_streak = 0, last_max_false_streak = 0;
	static int post_boot_indication = 0; static bool idle_os = 0;
	static const char *sleep_file = "sleep_criteria.txt";
	static struct SleepCriteria sleep_criteria = {0}; // TODO - this needs to be changed to take spectral samples and compare with the baselines (for idle and busy).

    // Initialize bands if needed
    if (!bands_initialized) {
        for (int i = 0; i < HISTORY_SECONDS; i++) {
            initialize_bands(system_state.true_intervals[i].band_boundaries);
            initialize_bands(system_state.false_intervals[i].band_boundaries);
            initialize_bands(system_state.total_intervals[i].band_boundaries);
        }
        csv_file = fopen("timing_spectrum.csv", "w");
        if (csv_file) {
            fprintf(csv_file, "Timestamp,VMState,IntervalType,Band,LowerBound,UpperBound,Count\n");
        }
        bands_initialized = true;
    }

    time_t current_time = time(NULL);

	// Check VM state every second
    if (current_time != last_vm_state_check) {
        FILE *state_file = fopen("vm_state.txt", "r");
        if (state_file) {
            int new_state;
            if (fscanf(state_file, "%d", &new_state) == 1)
                system_state.current_vm_state = new_state;
			else // 2 is neither busy(1) or idle(0)
                system_state.current_vm_state = 2;
            fclose(state_file);
        }
        last_vm_state_check = current_time;
    }

    // Every 20 seconds, disable sleep for a 1 second to measure
	if (current_time - last_measure_time >= 20) {
		measuring_mode = true;
		last_measure_time = current_time;
		sleep_enabled = false;
	} else if (measuring_mode && current_time - last_measure_time >= 1) {
		natural_true_rate = true_count;
		measuring_mode = false;
        sleep_enabled = true;
	}

    struct timespec current_ts;
    clock_gettime(CLOCK_MONOTONIC, &current_ts);

	if (system_state.current_vm_state < 2) {
		if (last_true_time.tv_sec != 0 || last_false_time.tv_sec != 0) {
			struct timespec *last_time = (last_true_time.tv_sec > last_false_time.tv_sec) ? &last_true_time : &last_false_time;
			long long interval = timespec_diff_ns(last_time, &current_ts);
			add_timing_sample(&system_state.total_intervals[system_state.current_index], interval);
		}
	}

    if (result) {
        true_count++;
        current_true_streak++;
        if (current_false_streak)
            if (!min_false_streak || current_false_streak < min_false_streak)
                min_false_streak = current_false_streak;
        current_false_streak = 0; // Reset false streak

        if (current_true_streak > max_true_streak)
            max_true_streak = current_true_streak;

        // True interval calculation
        if (last_true_time.tv_sec != 0) {
            long long interval = timespec_diff_ns(&last_true_time, &current_ts);
			if (system_state.current_vm_state < 2)
				add_timing_sample(&system_state.true_intervals[system_state.current_index], interval);
            true_interval_ns += interval;
            min_true_interval = (min_true_interval == 0) ?
                interval : (interval < min_true_interval ? interval : min_true_interval);
            max_true_interval = (interval > max_true_interval) ? interval : max_true_interval;

            erm_sleep = to_sleep;
			if (sleep_enabled && (should_sleep(sleep_criteria, true, interval) || erm_sleep > to_sleep)) {
                sleeps++;
                struct timespec sleep_duration = {0, erm_sleep * 1000};
                timespecadd(&last_false_time, &sleep_duration);
                timespecadd(&last_true_time, &sleep_duration);
                usleep(erm_sleep);
			}
        }
        memcpy(&last_true_time, &current_ts, sizeof(struct timespec));
    } else {
        false_count++;
        current_false_streak++;
        if (current_true_streak)
            if (!min_true_streak || current_true_streak < min_true_streak)
                min_true_streak = current_true_streak;
        current_true_streak = 0; // Reset true streak

        if (current_false_streak > max_false_streak)
            max_false_streak = current_false_streak;

        // False interval calculation
        if (last_false_time.tv_sec != 0) {
            long long interval = timespec_diff_ns(&last_false_time, &current_ts);
			if (system_state.current_vm_state < 2)
				add_timing_sample(&system_state.false_intervals[system_state.current_index], interval);
            false_interval_ns += interval;
            min_false_interval = (min_false_interval == 0) ?
                interval : (interval < min_false_interval ? interval : min_false_interval);
            max_false_interval = (interval > max_false_interval) ? interval : max_false_interval;

            erm_sleep = to_sleep;
            if (interval < 520) {
                if (((last_max_false_streak >= 540 && last_true_count > 10 && post_boot_indication <= 2) || (last_max_false_streak >= 450 && last_true_count > 15 && post_boot_indication > 2)) && last_max_false_streak <= 600 && last_max_true_streak == 1) {
					if (post_boot_indication < 200) post_boot_indication++;
				} else {
					post_boot_indication = 0;
					if (last_max_true_streak == 2 && interval < 342 && last_max_false_streak < 41 && last_min_false_streak < 4)
						idle_os = 1;
				}
			} else if (last_min_false_interval > 520) idle_os = 0;
			if (post_boot_indication > 2)
				erm_sleep = 100000;
			if (sleep_enabled && (should_sleep(sleep_criteria, false, interval) || erm_sleep > to_sleep)) {
                sleeps++;
                struct timespec sleep_duration = {0, erm_sleep * 1000};
                timespecadd(&last_false_time, &sleep_duration);
                timespecadd(&last_true_time, &sleep_duration);
                usleep(erm_sleep);
			}
        }
        memcpy(&last_false_time, &current_ts, sizeof(struct timespec));
    }


    // Print stats every second
    if (current_time != last_print_time) {
		if (system_state.current_vm_state < 2) {
			if (csv_file) {
				output_csv(csv_file, &system_state);
				fflush(csv_file);
			}

			system_state.current_index = (system_state.current_index + 1) % HISTORY_SECONDS;
			memset(&system_state.true_intervals[system_state.current_index], 0, sizeof(struct TimingSpectrum));
			memset(&system_state.false_intervals[system_state.current_index], 0, sizeof(struct TimingSpectrum));
			memset(&system_state.total_intervals[system_state.current_index], 0, sizeof(struct TimingSpectrum));
			initialize_bands(system_state.true_intervals[system_state.current_index].band_boundaries);
			initialize_bands(system_state.false_intervals[system_state.current_index].band_boundaries);
			initialize_bands(system_state.total_intervals[system_state.current_index].band_boundaries);
			system_state.true_intervals[system_state.current_index].timestamp = current_time;
			system_state.false_intervals[system_state.current_index].timestamp = current_time;
			system_state.total_intervals[system_state.current_index].timestamp = current_time;
		}

		sleep_criteria = parse_sleep_file(sleep_file);

        printf("Interrupt Stats: %s%s\n"
			   "Sleep Criteria: %s on %s %s %lld ns\n"
               "  Counts - True: %u, False: %u, to_sleep: %lu, sleeps: %u\n"
               "  Avg True Interval: %llu ns (Min/Max: %llu/%llu)\n"
               "  Avg False Interval: %llu ns (Min/Max: %llu/%llu)\n"
               "  True Streaks: Min: %d, Max: %d\n"
               "  False Streaks: Min: %d, Max: %d\n",
			   idle_os ? "idle_os " : "", post_boot_indication > 2 ? "post-boot" : "",
			   sleep_criteria.valid ? "Active" : "Invalid",
			   sleep_criteria.on_true ? "TRUE" : "FALSE",
			   sleep_criteria.compare_over ? "OVER" : "BELOW",
			   sleep_criteria.threshold,
               true_count, false_count, to_sleep, sleeps,
               true_count ? true_interval_ns / true_count : 0,
               min_true_interval, max_true_interval,
               false_count ? false_interval_ns / false_count : 0,
               min_false_interval, max_false_interval,
               min_true_streak, max_true_streak,
               min_false_streak, max_false_streak);

        if (sleeps) {
            if (true_count < natural_true_rate / 2) to_sleep = to_sleep * 99 / 100;
            else if (true_count >= natural_true_rate) to_sleep = to_sleep * 100 / 99;
        }

        last_true_count = true_count;
        sleeps = 0; true_count = 0; false_count = 0;
        true_interval_ns = 0; false_interval_ns = 0;
        min_true_interval = 0; max_true_interval = 0;
		last_min_false_interval = min_false_interval;
        min_false_interval = 0; max_false_interval = 0;
		last_max_true_streak = max_true_streak;
        min_true_streak = 0; max_true_streak = 0;
        last_min_false_streak = min_false_streak;
        last_max_false_streak = max_false_streak;
        min_false_streak = 0; max_false_streak = 0;

        last_print_time = current_time;
    }

    return result;
}
#endif /* !CONFIG_USER_ONLY */

static void cpu_sparc_disas_set_info(CPUState *cpu, disassemble_info *info)
{
    info->print_insn = print_insn_sparc;
#ifdef TARGET_SPARC64
    info->mach = bfd_mach_sparc_v9b;
#endif
}

static void
cpu_add_feat_as_prop(const char *typename, const char *name, const char *val)
{
    GlobalProperty *prop = g_new0(typeof(*prop), 1);
    prop->driver = typename;
    prop->property = g_strdup(name);
    prop->value = g_strdup(val);
    qdev_prop_register_global(prop);
}

/* Parse "+feature,-feature,feature=foo" CPU feature string */
static void sparc_cpu_parse_features(const char *typename, char *features,
                                     Error **errp)
{
    GList *l, *plus_features = NULL, *minus_features = NULL;
    char *featurestr; /* Single 'key=value" string being parsed */
    static bool cpu_globals_initialized;

    if (cpu_globals_initialized) {
        return;
    }
    cpu_globals_initialized = true;

    if (!features) {
        return;
    }

    for (featurestr = strtok(features, ",");
         featurestr;
         featurestr = strtok(NULL, ",")) {
        const char *name;
        const char *val = NULL;
        char *eq = NULL;

        /* Compatibility syntax: */
        if (featurestr[0] == '+') {
            plus_features = g_list_append(plus_features,
                                          g_strdup(featurestr + 1));
            continue;
        } else if (featurestr[0] == '-') {
            minus_features = g_list_append(minus_features,
                                           g_strdup(featurestr + 1));
            continue;
        }

        eq = strchr(featurestr, '=');
        name = featurestr;
        if (eq) {
            *eq++ = 0;
            val = eq;

            /*
             * Temporarily, only +feat/-feat will be supported
             * for boolean properties until we remove the
             * minus-overrides-plus semantics and just follow
             * the order options appear on the command-line.
             *
             * TODO: warn if user is relying on minus-override-plus semantics
             * TODO: remove minus-override-plus semantics after
             *       warning for a few releases
             */
            if (!strcasecmp(val, "on") ||
                !strcasecmp(val, "off") ||
                !strcasecmp(val, "true") ||
                !strcasecmp(val, "false")) {
                error_setg(errp, "Boolean properties in format %s=%s"
                                 " are not supported", name, val);
                return;
            }
        } else {
            error_setg(errp, "Unsupported property format: %s", name);
            return;
        }
        cpu_add_feat_as_prop(typename, name, val);
    }

    for (l = plus_features; l; l = l->next) {
        const char *name = l->data;
        cpu_add_feat_as_prop(typename, name, "on");
    }
    g_list_free_full(plus_features, g_free);

    for (l = minus_features; l; l = l->next) {
        const char *name = l->data;
        cpu_add_feat_as_prop(typename, name, "off");
    }
    g_list_free_full(minus_features, g_free);
}

void cpu_sparc_set_id(CPUSPARCState *env, unsigned int cpu)
{
#if !defined(TARGET_SPARC64)
    env->mxccregs[7] = ((cpu + 8) & 0xf) << 24;
#endif
}

static const sparc_def_t sparc_defs[] = {
#ifdef TARGET_SPARC64
    {
        .name = "Fujitsu-Sparc64",
        .iu_version = ((0x04ULL << 48) | (0x02ULL << 32) | (0ULL << 24)),
        .fpu_version = 0x00000000,
        .mmu_version = mmu_us_12,
        .nwindows = 4,
        .maxtl = 4,
        .features = CPU_DEFAULT_FEATURES,
    },
    {
        .name = "Fujitsu-Sparc64-III",
        .iu_version = ((0x04ULL << 48) | (0x03ULL << 32) | (0ULL << 24)),
        .fpu_version = 0x00000000,
        .mmu_version = mmu_us_12,
        .nwindows = 5,
        .maxtl = 4,
        .features = CPU_DEFAULT_FEATURES,
    },
    {
        .name = "Fujitsu-Sparc64-IV",
        .iu_version = ((0x04ULL << 48) | (0x04ULL << 32) | (0ULL << 24)),
        .fpu_version = 0x00000000,
        .mmu_version = mmu_us_12,
        .nwindows = 8,
        .maxtl = 5,
        .features = CPU_DEFAULT_FEATURES,
    },
    {
        .name = "Fujitsu-Sparc64-V",
        .iu_version = ((0x04ULL << 48) | (0x05ULL << 32) | (0x51ULL << 24)),
        .fpu_version = 0x00000000,
        .mmu_version = mmu_us_12,
        .nwindows = 8,
        .maxtl = 5,
        .features = CPU_DEFAULT_FEATURES,
    },
    {
        .name = "TI-UltraSparc-I",
        .iu_version = ((0x17ULL << 48) | (0x10ULL << 32) | (0x40ULL << 24)),
        .fpu_version = 0x00000000,
        .mmu_version = mmu_us_12,
        .nwindows = 8,
        .maxtl = 5,
        .features = CPU_DEFAULT_FEATURES,
    },
    {
        .name = "TI-UltraSparc-II",
        .iu_version = ((0x17ULL << 48) | (0x11ULL << 32) | (0x20ULL << 24)),
        .fpu_version = 0x00000000,
        .mmu_version = mmu_us_12,
        .nwindows = 8,
        .maxtl = 5,
        .features = CPU_DEFAULT_FEATURES,
    },
    {
        .name = "TI-UltraSparc-IIi",
        .iu_version = ((0x17ULL << 48) | (0x12ULL << 32) | (0x91ULL << 24)),
        .fpu_version = 0x00000000,
        .mmu_version = mmu_us_12,
        .nwindows = 8,
        .maxtl = 5,
        .features = CPU_DEFAULT_FEATURES,
    },
    {
        .name = "TI-UltraSparc-IIe",
        .iu_version = ((0x17ULL << 48) | (0x13ULL << 32) | (0x14ULL << 24)),
        .fpu_version = 0x00000000,
        .mmu_version = mmu_us_12,
        .nwindows = 8,
        .maxtl = 5,
        .features = CPU_DEFAULT_FEATURES,
    },
    {
        .name = "Sun-UltraSparc-III",
        .iu_version = ((0x3eULL << 48) | (0x14ULL << 32) | (0x34ULL << 24)),
        .fpu_version = 0x00000000,
        .mmu_version = mmu_us_12,
        .nwindows = 8,
        .maxtl = 5,
        .features = CPU_DEFAULT_FEATURES,
    },
    {
        .name = "Sun-UltraSparc-III-Cu",
        .iu_version = ((0x3eULL << 48) | (0x15ULL << 32) | (0x41ULL << 24)),
        .fpu_version = 0x00000000,
        .mmu_version = mmu_us_3,
        .nwindows = 8,
        .maxtl = 5,
        .features = CPU_DEFAULT_FEATURES,
    },
    {
        .name = "Sun-UltraSparc-IIIi",
        .iu_version = ((0x3eULL << 48) | (0x16ULL << 32) | (0x34ULL << 24)),
        .fpu_version = 0x00000000,
        .mmu_version = mmu_us_12,
        .nwindows = 8,
        .maxtl = 5,
        .features = CPU_DEFAULT_FEATURES,
    },
    {
        .name = "Sun-UltraSparc-IV",
        .iu_version = ((0x3eULL << 48) | (0x18ULL << 32) | (0x31ULL << 24)),
        .fpu_version = 0x00000000,
        .mmu_version = mmu_us_4,
        .nwindows = 8,
        .maxtl = 5,
        .features = CPU_DEFAULT_FEATURES,
    },
    {
        .name = "Sun-UltraSparc-IV-plus",
        .iu_version = ((0x3eULL << 48) | (0x19ULL << 32) | (0x22ULL << 24)),
        .fpu_version = 0x00000000,
        .mmu_version = mmu_us_12,
        .nwindows = 8,
        .maxtl = 5,
        .features = CPU_DEFAULT_FEATURES | CPU_FEATURE_CMT,
    },
    {
        .name = "Sun-UltraSparc-IIIi-plus",
        .iu_version = ((0x3eULL << 48) | (0x22ULL << 32) | (0ULL << 24)),
        .fpu_version = 0x00000000,
        .mmu_version = mmu_us_3,
        .nwindows = 8,
        .maxtl = 5,
        .features = CPU_DEFAULT_FEATURES,
    },
    {
        .name = "Sun-UltraSparc-T1",
        /* defined in sparc_ifu_fdp.v and ctu.h */
        .iu_version = ((0x3eULL << 48) | (0x23ULL << 32) | (0x02ULL << 24)),
        .fpu_version = 0x00000000,
        .mmu_version = mmu_sun4v,
        .nwindows = 8,
        .maxtl = 6,
        .features = CPU_DEFAULT_FEATURES | CPU_FEATURE_HYPV | CPU_FEATURE_CMT
        | CPU_FEATURE_GL,
    },
    {
        .name = "Sun-UltraSparc-T2",
        /* defined in tlu_asi_ctl.v and n2_revid_cust.v */
        .iu_version = ((0x3eULL << 48) | (0x24ULL << 32) | (0x02ULL << 24)),
        .fpu_version = 0x00000000,
        .mmu_version = mmu_sun4v,
        .nwindows = 8,
        .maxtl = 6,
        .features = CPU_DEFAULT_FEATURES | CPU_FEATURE_HYPV | CPU_FEATURE_CMT
        | CPU_FEATURE_GL,
    },
    {
        .name = "NEC-UltraSparc-I",
        .iu_version = ((0x22ULL << 48) | (0x10ULL << 32) | (0x40ULL << 24)),
        .fpu_version = 0x00000000,
        .mmu_version = mmu_us_12,
        .nwindows = 8,
        .maxtl = 5,
        .features = CPU_DEFAULT_FEATURES,
    },
#else
    {
        .name = "Fujitsu-MB86904",
        .iu_version = 0x04 << 24, /* Impl 0, ver 4 */
        .fpu_version = 4 << FSR_VER_SHIFT, /* FPU version 4 (Meiko) */
        .mmu_version = 0x04 << 24, /* Impl 0, ver 4 */
        .mmu_bm = 0x00004000,
        .mmu_ctpr_mask = 0x00ffffc0,
        .mmu_cxr_mask = 0x000000ff,
        .mmu_sfsr_mask = 0x00016fff,
        .mmu_trcr_mask = 0x00ffffff,
        .nwindows = 8,
        .features = CPU_DEFAULT_FEATURES,
    },
    {
        .name = "Fujitsu-MB86907",
        .iu_version = 0x05 << 24, /* Impl 0, ver 5 */
        .fpu_version = 4 << FSR_VER_SHIFT, /* FPU version 4 (Meiko) */
        .mmu_version = 0x05 << 24, /* Impl 0, ver 5 */
        .mmu_bm = 0x00004000,
        .mmu_ctpr_mask = 0xffffffc0,
        .mmu_cxr_mask = 0x000000ff,
        .mmu_sfsr_mask = 0x00016fff,
        .mmu_trcr_mask = 0xffffffff,
        .nwindows = 8,
        .features = CPU_DEFAULT_FEATURES,
    },
    {
        .name = "TI-MicroSparc-I",
        .iu_version = 0x41000000,
        .fpu_version = 4 << FSR_VER_SHIFT,
        .mmu_version = 0x41000000,
        .mmu_bm = 0x00004000,
        .mmu_ctpr_mask = 0x007ffff0,
        .mmu_cxr_mask = 0x0000003f,
        .mmu_sfsr_mask = 0x00016fff,
        .mmu_trcr_mask = 0x0000003f,
        .nwindows = 7,
        .features = CPU_FEATURE_MUL | CPU_FEATURE_DIV,
    },
    {
        .name = "TI-MicroSparc-II",
        .iu_version = 0x42000000,
        .fpu_version = 4 << FSR_VER_SHIFT,
        .mmu_version = 0x02000000,
        .mmu_bm = 0x00004000,
        .mmu_ctpr_mask = 0x00ffffc0,
        .mmu_cxr_mask = 0x000000ff,
        .mmu_sfsr_mask = 0x00016fff,
        .mmu_trcr_mask = 0x00ffffff,
        .nwindows = 8,
        .features = CPU_DEFAULT_FEATURES,
    },
    {
        .name = "TI-MicroSparc-IIep",
        .iu_version = 0x42000000,
        .fpu_version = 4 << FSR_VER_SHIFT,
        .mmu_version = 0x04000000,
        .mmu_bm = 0x00004000,
        .mmu_ctpr_mask = 0x00ffffc0,
        .mmu_cxr_mask = 0x000000ff,
        .mmu_sfsr_mask = 0x00016bff,
        .mmu_trcr_mask = 0x00ffffff,
        .nwindows = 8,
        .features = CPU_DEFAULT_FEATURES,
    },
    {
        .name = "TI-SuperSparc-40", /* STP1020NPGA */
        .iu_version = 0x41000000, /* SuperSPARC 2.x */
        .fpu_version = 0 << FSR_VER_SHIFT,
        .mmu_version = 0x00000800, /* SuperSPARC 2.x, no MXCC */
        .mmu_bm = 0x00002000,
        .mmu_ctpr_mask = 0xffffffc0,
        .mmu_cxr_mask = 0x0000ffff,
        .mmu_sfsr_mask = 0xffffffff,
        .mmu_trcr_mask = 0xffffffff,
        .nwindows = 8,
        .features = CPU_DEFAULT_FEATURES,
    },
    {
        .name = "TI-SuperSparc-50", /* STP1020PGA */
        .iu_version = 0x40000000, /* SuperSPARC 3.x */
        .fpu_version = 0 << FSR_VER_SHIFT,
        .mmu_version = 0x01000800, /* SuperSPARC 3.x, no MXCC */
        .mmu_bm = 0x00002000,
        .mmu_ctpr_mask = 0xffffffc0,
        .mmu_cxr_mask = 0x0000ffff,
        .mmu_sfsr_mask = 0xffffffff,
        .mmu_trcr_mask = 0xffffffff,
        .nwindows = 8,
        .features = CPU_DEFAULT_FEATURES,
    },
    {
        .name = "TI-SuperSparc-51",
        .iu_version = 0x40000000, /* SuperSPARC 3.x */
        .fpu_version = 0 << FSR_VER_SHIFT,
        .mmu_version = 0x01000000, /* SuperSPARC 3.x, MXCC */
        .mmu_bm = 0x00002000,
        .mmu_ctpr_mask = 0xffffffc0,
        .mmu_cxr_mask = 0x0000ffff,
        .mmu_sfsr_mask = 0xffffffff,
        .mmu_trcr_mask = 0xffffffff,
        .mxcc_version = 0x00000104,
        .nwindows = 8,
        .features = CPU_DEFAULT_FEATURES,
    },
    {
        .name = "TI-SuperSparc-60", /* STP1020APGA */
        .iu_version = 0x40000000, /* SuperSPARC 3.x */
        .fpu_version = 0 << FSR_VER_SHIFT,
        .mmu_version = 0x01000800, /* SuperSPARC 3.x, no MXCC */
        .mmu_bm = 0x00002000,
        .mmu_ctpr_mask = 0xffffffc0,
        .mmu_cxr_mask = 0x0000ffff,
        .mmu_sfsr_mask = 0xffffffff,
        .mmu_trcr_mask = 0xffffffff,
        .nwindows = 8,
        .features = CPU_DEFAULT_FEATURES,
    },
    {
        .name = "TI-SuperSparc-61",
        .iu_version = 0x44000000, /* SuperSPARC 3.x */
        .fpu_version = 0 << FSR_VER_SHIFT,
        .mmu_version = 0x01000000, /* SuperSPARC 3.x, MXCC */
        .mmu_bm = 0x00002000,
        .mmu_ctpr_mask = 0xffffffc0,
        .mmu_cxr_mask = 0x0000ffff,
        .mmu_sfsr_mask = 0xffffffff,
        .mmu_trcr_mask = 0xffffffff,
        .mxcc_version = 0x00000104,
        .nwindows = 8,
        .features = CPU_DEFAULT_FEATURES,
    },
    {
        .name = "TI-SuperSparc-II",
        .iu_version = 0x40000000, /* SuperSPARC II 1.x */
        .fpu_version = 0 << FSR_VER_SHIFT,
        .mmu_version = 0x08000000, /* SuperSPARC II 1.x, MXCC */
        .mmu_bm = 0x00002000,
        .mmu_ctpr_mask = 0xffffffc0,
        .mmu_cxr_mask = 0x0000ffff,
        .mmu_sfsr_mask = 0xffffffff,
        .mmu_trcr_mask = 0xffffffff,
        .mxcc_version = 0x00000104,
        .nwindows = 8,
        .features = CPU_DEFAULT_FEATURES,
    },
    {
        .name = "LEON2",
        .iu_version = 0xf2000000,
        .fpu_version = 4 << FSR_VER_SHIFT, /* FPU version 4 (Meiko) */
        .mmu_version = 0xf2000000,
        .mmu_bm = 0x00004000,
        .mmu_ctpr_mask = 0x007ffff0,
        .mmu_cxr_mask = 0x0000003f,
        .mmu_sfsr_mask = 0xffffffff,
        .mmu_trcr_mask = 0xffffffff,
        .nwindows = 8,
        .features = CPU_DEFAULT_FEATURES | CPU_FEATURE_TA0_SHUTDOWN,
    },
    {
        .name = "LEON3",
        .iu_version = 0xf3000000,
        .fpu_version = 4 << FSR_VER_SHIFT, /* FPU version 4 (Meiko) */
        .mmu_version = 0xf3000000,
        .mmu_bm = 0x00000000,
        .mmu_ctpr_mask = 0xfffffffc,
        .mmu_cxr_mask = 0x000000ff,
        .mmu_sfsr_mask = 0xffffffff,
        .mmu_trcr_mask = 0xffffffff,
        .nwindows = 8,
        .features = CPU_DEFAULT_FEATURES | CPU_FEATURE_TA0_SHUTDOWN |
        CPU_FEATURE_ASR17 | CPU_FEATURE_CACHE_CTRL | CPU_FEATURE_POWERDOWN |
        CPU_FEATURE_CASA,
    },
#endif
};

/* This must match sparc_cpu_properties[]. */
static const char * const feature_name[] = {
    [CPU_FEATURE_BIT_FLOAT128] = "float128",
#ifdef TARGET_SPARC64
    [CPU_FEATURE_BIT_CMT] = "cmt",
    [CPU_FEATURE_BIT_GL] = "gl",
    [CPU_FEATURE_BIT_HYPV] = "hypv",
    [CPU_FEATURE_BIT_VIS1] = "vis1",
    [CPU_FEATURE_BIT_VIS2] = "vis2",
    [CPU_FEATURE_BIT_FMAF] = "fmaf",
    [CPU_FEATURE_BIT_VIS3] = "vis3",
    [CPU_FEATURE_BIT_IMA] = "ima",
    [CPU_FEATURE_BIT_VIS4] = "vis4",
#else
    [CPU_FEATURE_BIT_MUL] = "mul",
    [CPU_FEATURE_BIT_DIV] = "div",
    [CPU_FEATURE_BIT_FSMULD] = "fsmuld",
#endif
};

static void print_features(uint32_t features, const char *prefix)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(feature_name); i++) {
        if (feature_name[i] && (features & (1 << i))) {
            if (prefix) {
                qemu_printf("%s", prefix);
            }
            qemu_printf("%s ", feature_name[i]);
        }
    }
}

void sparc_cpu_list(void)
{
    unsigned int i;

    qemu_printf("Available CPU types:\n");
    for (i = 0; i < ARRAY_SIZE(sparc_defs); i++) {
        qemu_printf(" %-20s (IU " TARGET_FMT_lx
                    " FPU %08x MMU %08x NWINS %d) ",
                    sparc_defs[i].name,
                    sparc_defs[i].iu_version,
                    sparc_defs[i].fpu_version,
                    sparc_defs[i].mmu_version,
                    sparc_defs[i].nwindows);
        print_features(CPU_DEFAULT_FEATURES & ~sparc_defs[i].features, "-");
        print_features(~CPU_DEFAULT_FEATURES & sparc_defs[i].features, "+");
        qemu_printf("\n");
    }
    qemu_printf("Default CPU feature flags (use '-' to remove): ");
    print_features(CPU_DEFAULT_FEATURES, NULL);
    qemu_printf("\n");
    qemu_printf("Available CPU feature flags (use '+' to add): ");
    print_features(~CPU_DEFAULT_FEATURES, NULL);
    qemu_printf("\n");
    qemu_printf("Numerical features (use '=' to set): iu_version "
                "fpu_version mmu_version nwindows\n");
}

static void cpu_print_cc(FILE *f, uint32_t cc)
{
    qemu_fprintf(f, "%c%c%c%c", cc & PSR_NEG ? 'N' : '-',
                 cc & PSR_ZERO ? 'Z' : '-', cc & PSR_OVF ? 'V' : '-',
                 cc & PSR_CARRY ? 'C' : '-');
}

#ifdef TARGET_SPARC64
#define REGS_PER_LINE 4
#else
#define REGS_PER_LINE 8
#endif

static void sparc_cpu_dump_state(CPUState *cs, FILE *f, int flags)
{
    CPUSPARCState *env = cpu_env(cs);
    int i, x;

    qemu_fprintf(f, "pc: " TARGET_FMT_lx "  npc: " TARGET_FMT_lx "\n", env->pc,
                 env->npc);

    for (i = 0; i < 8; i++) {
        if (i % REGS_PER_LINE == 0) {
            qemu_fprintf(f, "%%g%d-%d:", i, i + REGS_PER_LINE - 1);
        }
        qemu_fprintf(f, " " TARGET_FMT_lx, env->gregs[i]);
        if (i % REGS_PER_LINE == REGS_PER_LINE - 1) {
            qemu_fprintf(f, "\n");
        }
    }
    for (x = 0; x < 3; x++) {
        for (i = 0; i < 8; i++) {
            if (i % REGS_PER_LINE == 0) {
                qemu_fprintf(f, "%%%c%d-%d: ",
                             x == 0 ? 'o' : (x == 1 ? 'l' : 'i'),
                             i, i + REGS_PER_LINE - 1);
            }
            qemu_fprintf(f, TARGET_FMT_lx " ", env->regwptr[i + x * 8]);
            if (i % REGS_PER_LINE == REGS_PER_LINE - 1) {
                qemu_fprintf(f, "\n");
            }
        }
    }

    if (flags & CPU_DUMP_FPU) {
        for (i = 0; i < TARGET_DPREGS; i++) {
            if ((i & 3) == 0) {
                qemu_fprintf(f, "%%f%02d: ", i * 2);
            }
            qemu_fprintf(f, " %016" PRIx64, env->fpr[i].ll);
            if ((i & 3) == 3) {
                qemu_fprintf(f, "\n");
            }
        }
    }

#ifdef TARGET_SPARC64
    qemu_fprintf(f, "pstate: %08x ccr: %02x (icc: ", env->pstate,
                 (unsigned)cpu_get_ccr(env));
    cpu_print_cc(f, cpu_get_ccr(env) << PSR_CARRY_SHIFT);
    qemu_fprintf(f, " xcc: ");
    cpu_print_cc(f, cpu_get_ccr(env) << (PSR_CARRY_SHIFT - 4));
    qemu_fprintf(f, ") asi: %02x tl: %d pil: %x gl: %d\n", env->asi, env->tl,
                 env->psrpil, env->gl);
    qemu_fprintf(f, "tbr: " TARGET_FMT_lx " hpstate: " TARGET_FMT_lx " htba: "
                 TARGET_FMT_lx "\n", env->tbr, env->hpstate, env->htba);
    qemu_fprintf(f, "cansave: %d canrestore: %d otherwin: %d wstate: %d "
                 "cleanwin: %d cwp: %d\n",
                 env->cansave, env->canrestore, env->otherwin, env->wstate,
                 env->cleanwin, env->nwindows - 1 - env->cwp);
    qemu_fprintf(f, "fsr: " TARGET_FMT_lx " y: " TARGET_FMT_lx " fprs: %016x\n",
                 cpu_get_fsr(env), env->y, env->fprs);

#else
    qemu_fprintf(f, "psr: %08x (icc: ", cpu_get_psr(env));
    cpu_print_cc(f, cpu_get_psr(env));
    qemu_fprintf(f, " SPE: %c%c%c) wim: %08x\n", env->psrs ? 'S' : '-',
                 env->psrps ? 'P' : '-', env->psret ? 'E' : '-',
                 env->wim);
    qemu_fprintf(f, "fsr: " TARGET_FMT_lx " y: " TARGET_FMT_lx "\n",
                 cpu_get_fsr(env), env->y);
#endif
    qemu_fprintf(f, "\n");
}

static void sparc_cpu_set_pc(CPUState *cs, vaddr value)
{
    SPARCCPU *cpu = SPARC_CPU(cs);

    cpu->env.pc = value;
    cpu->env.npc = value + 4;
}

static vaddr sparc_cpu_get_pc(CPUState *cs)
{
    SPARCCPU *cpu = SPARC_CPU(cs);

    return cpu->env.pc;
}

static void sparc_cpu_synchronize_from_tb(CPUState *cs,
                                          const TranslationBlock *tb)
{
    SPARCCPU *cpu = SPARC_CPU(cs);

    tcg_debug_assert(!tcg_cflags_has(cs, CF_PCREL));
    cpu->env.pc = tb->pc;
    cpu->env.npc = tb->cs_base;
}

void cpu_get_tb_cpu_state(CPUSPARCState *env, vaddr *pc,
                          uint64_t *cs_base, uint32_t *pflags)
{
    uint32_t flags;
    *pc = env->pc;
    *cs_base = env->npc;
    flags = cpu_mmu_index(env_cpu(env), false);
#ifndef CONFIG_USER_ONLY
    if (cpu_supervisor_mode(env)) {
        flags |= TB_FLAG_SUPER;
    }
#endif
#ifdef TARGET_SPARC64
#ifndef CONFIG_USER_ONLY
    if (cpu_hypervisor_mode(env)) {
        flags |= TB_FLAG_HYPER;
    }
#endif
    if (env->pstate & PS_AM) {
        flags |= TB_FLAG_AM_ENABLED;
    }
    if ((env->pstate & PS_PEF) && (env->fprs & FPRS_FEF)) {
        flags |= TB_FLAG_FPU_ENABLED;
    }
    flags |= env->asi << TB_FLAG_ASI_SHIFT;
#else
    if (env->psref) {
        flags |= TB_FLAG_FPU_ENABLED;
    }
#ifndef CONFIG_USER_ONLY
    if (env->fsr_qne) {
        flags |= TB_FLAG_FSR_QNE;
    }
#endif /* !CONFIG_USER_ONLY */
#endif /* TARGET_SPARC64 */
    *pflags = flags;
}

static void sparc_restore_state_to_opc(CPUState *cs,
                                       const TranslationBlock *tb,
                                       const uint64_t *data)
{
    CPUSPARCState *env = cpu_env(cs);
    target_ulong pc = data[0];
    target_ulong npc = data[1];

    env->pc = pc;
    if (npc == DYNAMIC_PC) {
        /* dynamic NPC: already stored */
    } else if (npc & JUMP_PC) {
        /* jump PC: use 'cond' and the jump targets of the translation */
        if (env->cond) {
            env->npc = npc & ~3;
        } else {
            env->npc = pc + 4;
        }
    } else {
        env->npc = npc;
    }
}

static bool sparc_cpu_has_work(CPUState *cs)
{
    static unsigned long long true_count = 0, false_count = 0;
    static time_t last_print_time = 0;
    bool result = (cs->interrupt_request & CPU_INTERRUPT_HARD) && cpu_interrupts_enabled(cpu_env(cs));

    if (result) true_count++;
    else false_count++;

    time_t current_time = time(NULL);
    if (current_time != last_print_time) {
        fprintf(stderr, "REB has_work stats - True: %llu, False: %llu\n", true_count, false_count);
        last_print_time = current_time;
    }

    return result;
}

static int sparc_cpu_mmu_index(CPUState *cs, bool ifetch)
{
    CPUSPARCState *env = cpu_env(cs);

#ifndef TARGET_SPARC64
    if ((env->mmuregs[0] & MMU_E) == 0) { /* MMU disabled */
        return MMU_PHYS_IDX;
    } else {
        return env->psrs;
    }
#else
    /* IMMU or DMMU disabled.  */
    if (ifetch
        ? (env->lsu & IMMU_E) == 0 || (env->pstate & PS_RED) != 0
        : (env->lsu & DMMU_E) == 0) {
        return MMU_PHYS_IDX;
    } else if (cpu_hypervisor_mode(env)) {
        return MMU_PHYS_IDX;
    } else if (env->tl > 0) {
        return MMU_NUCLEUS_IDX;
    } else if (cpu_supervisor_mode(env)) {
        return MMU_KERNEL_IDX;
    } else {
        return MMU_USER_IDX;
    }
#endif
}

static char *sparc_cpu_type_name(const char *cpu_model)
{
    char *name = g_strdup_printf(SPARC_CPU_TYPE_NAME("%s"), cpu_model);
    char *s = name;

    /* SPARC cpu model names happen to have whitespaces,
     * as type names shouldn't have spaces replace them with '-'
     */
    while ((s = strchr(s, ' '))) {
        *s = '-';
    }

    return name;
}

static ObjectClass *sparc_cpu_class_by_name(const char *cpu_model)
{
    ObjectClass *oc;
    char *typename;

    typename = sparc_cpu_type_name(cpu_model);

    /* Fix up legacy names with '+' in it */
    if (g_str_equal(typename, SPARC_CPU_TYPE_NAME("Sun-UltraSparc-IV+"))) {
        g_free(typename);
        typename = g_strdup(SPARC_CPU_TYPE_NAME("Sun-UltraSparc-IV-plus"));
    } else if (g_str_equal(typename, SPARC_CPU_TYPE_NAME("Sun-UltraSparc-IIIi+"))) {
        g_free(typename);
        typename = g_strdup(SPARC_CPU_TYPE_NAME("Sun-UltraSparc-IIIi-plus"));
    }

    oc = object_class_by_name(typename);
    g_free(typename);
    return oc;
}

static void sparc_cpu_realizefn(DeviceState *dev, Error **errp)
{
    CPUState *cs = CPU(dev);
    SPARCCPUClass *scc = SPARC_CPU_GET_CLASS(dev);
    Error *local_err = NULL;
    CPUSPARCState *env = cpu_env(cs);

#if defined(CONFIG_USER_ONLY)
    /* We are emulating the kernel, which will trap and emulate float128. */
    env->def.features |= CPU_FEATURE_FLOAT128;
#endif

    env->version = env->def.iu_version;
    env->nwindows = env->def.nwindows;
#if !defined(TARGET_SPARC64)
    env->mmuregs[0] |= env->def.mmu_version;
    cpu_sparc_set_id(env, 0);
    env->mxccregs[7] |= env->def.mxcc_version;
#else
    env->mmu_version = env->def.mmu_version;
    env->maxtl = env->def.maxtl;
    env->version |= env->def.maxtl << 8;
    env->version |= env->def.nwindows - 1;
#endif

    /*
     * Prefer SNaN over QNaN, order B then A. It's OK to do this in realize
     * rather than reset, because fp_status is after 'end_reset_fields' in
     * the CPU state struct so it won't get zeroed on reset.
     */
    set_float_2nan_prop_rule(float_2nan_prop_s_ba, &env->fp_status);
    /* For fused-multiply add, prefer SNaN over QNaN, then C->B->A */
    set_float_3nan_prop_rule(float_3nan_prop_s_cba, &env->fp_status);
    /* For inf * 0 + NaN, return the input NaN */
    set_float_infzeronan_rule(float_infzeronan_dnan_never, &env->fp_status);
    /* Default NaN value: sign bit clear, all frac bits set */
    set_float_default_nan_pattern(0b01111111, &env->fp_status);

    cpu_exec_realizefn(cs, &local_err);
    if (local_err != NULL) {
        error_propagate(errp, local_err);
        return;
    }

    qemu_init_vcpu(cs);

    scc->parent_realize(dev, errp);
}

static void sparc_cpu_initfn(Object *obj)
{
    SPARCCPU *cpu = SPARC_CPU(obj);
    SPARCCPUClass *scc = SPARC_CPU_GET_CLASS(obj);
    CPUSPARCState *env = &cpu->env;

    if (scc->cpu_def) {
        env->def = *scc->cpu_def;
    }
}

static void sparc_get_nwindows(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    SPARCCPU *cpu = SPARC_CPU(obj);
    int64_t value = cpu->env.def.nwindows;

    visit_type_int(v, name, &value, errp);
}

static void sparc_set_nwindows(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    const int64_t min = MIN_NWINDOWS;
    const int64_t max = MAX_NWINDOWS;
    SPARCCPU *cpu = SPARC_CPU(obj);
    int64_t value;

    if (!visit_type_int(v, name, &value, errp)) {
        return;
    }

    if (value < min || value > max) {
        error_setg(errp, "Property %s.%s doesn't take value %" PRId64
                   " (minimum: %" PRId64 ", maximum: %" PRId64 ")",
                   object_get_typename(obj), name ? name : "null",
                   value, min, max);
        return;
    }
    cpu->env.def.nwindows = value;
}

static const PropertyInfo qdev_prop_nwindows = {
    .name  = "int",
    .get   = sparc_get_nwindows,
    .set   = sparc_set_nwindows,
};

/* This must match feature_name[]. */
static const Property sparc_cpu_properties[] = {
    DEFINE_PROP_BIT("float128", SPARCCPU, env.def.features,
                    CPU_FEATURE_BIT_FLOAT128, false),
#ifdef TARGET_SPARC64
    DEFINE_PROP_BIT("cmt",      SPARCCPU, env.def.features,
                    CPU_FEATURE_BIT_CMT, false),
    DEFINE_PROP_BIT("gl",       SPARCCPU, env.def.features,
                    CPU_FEATURE_BIT_GL, false),
    DEFINE_PROP_BIT("hypv",     SPARCCPU, env.def.features,
                    CPU_FEATURE_BIT_HYPV, false),
    DEFINE_PROP_BIT("vis1",     SPARCCPU, env.def.features,
                    CPU_FEATURE_BIT_VIS1, false),
    DEFINE_PROP_BIT("vis2",     SPARCCPU, env.def.features,
                    CPU_FEATURE_BIT_VIS2, false),
    DEFINE_PROP_BIT("fmaf",     SPARCCPU, env.def.features,
                    CPU_FEATURE_BIT_FMAF, false),
    DEFINE_PROP_BIT("vis3",     SPARCCPU, env.def.features,
                    CPU_FEATURE_BIT_VIS3, false),
    DEFINE_PROP_BIT("ima",      SPARCCPU, env.def.features,
                    CPU_FEATURE_BIT_IMA, false),
    DEFINE_PROP_BIT("vis4",     SPARCCPU, env.def.features,
                    CPU_FEATURE_BIT_VIS4, false),
#else
    DEFINE_PROP_BIT("mul",      SPARCCPU, env.def.features,
                    CPU_FEATURE_BIT_MUL, false),
    DEFINE_PROP_BIT("div",      SPARCCPU, env.def.features,
                    CPU_FEATURE_BIT_DIV, false),
    DEFINE_PROP_BIT("fsmuld",   SPARCCPU, env.def.features,
                    CPU_FEATURE_BIT_FSMULD, false),
#endif
    DEFINE_PROP_UNSIGNED("iu-version", SPARCCPU, env.def.iu_version, 0,
                         qdev_prop_uint64, target_ulong),
    DEFINE_PROP_UINT32("fpu-version", SPARCCPU, env.def.fpu_version, 0),
    DEFINE_PROP_UINT32("mmu-version", SPARCCPU, env.def.mmu_version, 0),
    DEFINE_PROP("nwindows", SPARCCPU, env.def.nwindows,
                qdev_prop_nwindows, uint32_t),
};

#ifndef CONFIG_USER_ONLY
#include "hw/core/sysemu-cpu-ops.h"

static const struct SysemuCPUOps sparc_sysemu_ops = {
    .get_phys_page_debug = sparc_cpu_get_phys_page_debug,
    .legacy_vmsd = &vmstate_sparc_cpu,
};
#endif

#ifdef CONFIG_TCG
#include "hw/core/tcg-cpu-ops.h"

static const TCGCPUOps sparc_tcg_ops = {
    .initialize = sparc_tcg_init,
    .translate_code = sparc_translate_code,
    .synchronize_from_tb = sparc_cpu_synchronize_from_tb,
    .restore_state_to_opc = sparc_restore_state_to_opc,

#ifndef CONFIG_USER_ONLY
    .tlb_fill = sparc_cpu_tlb_fill,
    .cpu_exec_interrupt = sparc_cpu_exec_interrupt,
    .cpu_exec_halt = sparc_cpu_has_work,
    .do_interrupt = sparc_cpu_do_interrupt,
    .do_transaction_failed = sparc_cpu_do_transaction_failed,
    .do_unaligned_access = sparc_cpu_do_unaligned_access,
#endif /* !CONFIG_USER_ONLY */
};
#endif /* CONFIG_TCG */

static void sparc_cpu_class_init(ObjectClass *oc, void *data)
{
    SPARCCPUClass *scc = SPARC_CPU_CLASS(oc);
    CPUClass *cc = CPU_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    device_class_set_parent_realize(dc, sparc_cpu_realizefn,
                                    &scc->parent_realize);
    device_class_set_props(dc, sparc_cpu_properties);

    resettable_class_set_parent_phases(rc, NULL, sparc_cpu_reset_hold, NULL,
                                       &scc->parent_phases);

    cc->class_by_name = sparc_cpu_class_by_name;
    cc->parse_features = sparc_cpu_parse_features;
    cc->has_work = sparc_cpu_has_work;
    cc->mmu_index = sparc_cpu_mmu_index;
    cc->dump_state = sparc_cpu_dump_state;
#if !defined(TARGET_SPARC64) && !defined(CONFIG_USER_ONLY)
    cc->memory_rw_debug = sparc_cpu_memory_rw_debug;
#endif
    cc->set_pc = sparc_cpu_set_pc;
    cc->get_pc = sparc_cpu_get_pc;
    cc->gdb_read_register = sparc_cpu_gdb_read_register;
    cc->gdb_write_register = sparc_cpu_gdb_write_register;
#ifndef CONFIG_USER_ONLY
    cc->sysemu_ops = &sparc_sysemu_ops;
#endif
    cc->disas_set_info = cpu_sparc_disas_set_info;

#if defined(TARGET_SPARC64) && !defined(TARGET_ABI32)
    cc->gdb_num_core_regs = 86;
#else
    cc->gdb_num_core_regs = 72;
#endif
    cc->tcg_ops = &sparc_tcg_ops;
}

static const TypeInfo sparc_cpu_type_info = {
    .name = TYPE_SPARC_CPU,
    .parent = TYPE_CPU,
    .instance_size = sizeof(SPARCCPU),
    .instance_align = __alignof(SPARCCPU),
    .instance_init = sparc_cpu_initfn,
    .abstract = true,
    .class_size = sizeof(SPARCCPUClass),
    .class_init = sparc_cpu_class_init,
};

static void sparc_cpu_cpudef_class_init(ObjectClass *oc, void *data)
{
    SPARCCPUClass *scc = SPARC_CPU_CLASS(oc);
    scc->cpu_def = data;
}

static void sparc_register_cpudef_type(const struct sparc_def_t *def)
{
    char *typename = sparc_cpu_type_name(def->name);
    TypeInfo ti = {
        .name = typename,
        .parent = TYPE_SPARC_CPU,
        .class_init = sparc_cpu_cpudef_class_init,
        .class_data = (void *)def,
    };

    type_register_static(&ti);
    g_free(typename);
}

static void sparc_cpu_register_types(void)
{
    int i;

    type_register_static(&sparc_cpu_type_info);
    for (i = 0; i < ARRAY_SIZE(sparc_defs); i++) {
        sparc_register_cpudef_type(&sparc_defs[i]);
    }
}

type_init(sparc_cpu_register_types)
