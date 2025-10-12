/*
 * QEMU CPU Idle Detection and Power Saving - Architecture-Agnostic Design
 *
 * This header documents the FUTURE architecture-agnostic interface for
 * idle loop detection. Currently implemented for SPARC in target/sparc/cpu.c.
 *
 * DESIGN GOALS:
 * - Learn PC/NPC patterns during idle and busy periods
 * - Detect guest idle loops at runtime
 * - Reduce host CPU usage via strategic g_usleep() calls
 * - Work across all architectures (x86, ARM, SPARC, 6502, etc.)
 *
 * TODO: Refactor target/sparc/cpu.c idle detection code to use this interface
 *       once SPARC implementation is stable and well-tested.
 *
 * Copyright (c) 2024 QEMU Contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef QEMU_CPU_IDLE_H
#define QEMU_CPU_IDLE_H

#include "exec/cpu-defs.h"  /* For target_ulong */
#include "hw/core/cpu.h"

/*
 * IMPLEMENTATION STATUS:
 * - Basic framework in system/cpu-idle.c (in progress)
 * - Full implementation still in target/sparc/cpu.c
 * - Gradual migration in progress
 */

/**
 * CPUPCState - Architecture-agnostic program counter state
 *
 * For pattern matching, we want to identify unique execution points.
 * Architectures handle this differently:
 *
 * - SPARC (delayed branch): Uses PC + NPC (both real registers)
 * - 6502/x86/ARM (immediate): Uses PC only, set next_pc = 0 to disable
 *
 * When next_pc is 0, idle detection uses only PC for pattern matching.
 */
typedef struct {
	target_ulong pc;       /* Current program counter */
	target_ulong next_pc;  /* Next PC (delayed branch archs) or 0 (immediate branch) */
} CPUPCState;

/**
 * cpu_get_pc_state - Get current PC state for a CPU
 * @cpu: The CPU to query
 * @state: Output structure to fill with PC values
 *
 * Architecture-specific function that must be implemented by each target.
 * Extracts the current and next program counter from the CPU state.
 */
void cpu_get_pc_state(CPUState *cpu, CPUPCState *state);

/**
 * cpu_idle_exec_hook - Main idle detection hook
 * @cpu: The CPU that is about to execute
 *
 * Called before each translation block execution. Performs:
 * - PC pattern learning (if in learning mode)
 * - Idle detection based on learned patterns
 * - Strategic sleeping to save host CPU
 * - Speed measurement and statistics
 *
 * Safe to call on all architectures. No-op if idle detection is disabled.
 */
void cpu_idle_exec_hook(CPUState *cpu);

/**
 * cpu_idle_set_debug - Enable/disable debug output
 * @enable: true to enable, false to disable
 *
 * Controls whether per-second statistics are printed to stderr.
 */
void cpu_idle_set_debug(bool enable);

/**
 * cpu_idle_start_prom_learning - Start PROM/firmware idle learning
 *
 * Begins collecting PC patterns while guest is in PROM/firmware idle state.
 * Auto-stops after 10,000 samples.
 */
void cpu_idle_start_prom_learning(void);

/**
 * cpu_idle_start_os_idle_learning - Start OS idle learning
 *
 * Begins collecting PC patterns while guest OS is idle (e.g., at login prompt).
 * Auto-stops after 10,000 samples.
 */
void cpu_idle_start_os_idle_learning(void);

/**
 * cpu_idle_start_busy_learning - Start busy/active learning
 *
 * Begins collecting PC patterns while guest OS is busy (compiling, etc.).
 * Used for cross-contamination filtering.
 * Auto-stops after 10,000 samples.
 */
void cpu_idle_start_busy_learning(void);

/**
 * cpu_idle_stop_learning - Stop current learning session
 *
 * Stops any active learning mode, displays results, performs confidence
 * adjustment, and saves learned patterns to disk.
 */
void cpu_idle_stop_learning(void);

/**
 * cpu_idle_init - Initialize idle detection system
 *
 * Called during QEMU startup. Loads previously learned patterns from disk.
 */
void cpu_idle_init(void);

/**
 * cpu_idle_register_fallback_pcs - Register hardcoded fallback idle PCs
 * @prom_pcs: Array of PROM/firmware idle PC values
 * @prom_count: Number of PROM idle PCs
 * @os_idle_pc: OS idle loop PC value
 *
 * Called by architecture code to provide fallback idle PCs when no learned
 * data is available. These are used as a starting point before learning.
 */
void cpu_idle_register_fallback_pcs(target_ulong *prom_pcs, int prom_count,
									 target_ulong os_idle_pc);

#endif /* QEMU_CPU_IDLE_H */

