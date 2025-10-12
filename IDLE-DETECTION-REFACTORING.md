# CPU Idle Detection - Architecture-Agnostic Refactoring Plan

## Current Status
- ✅ **Working**: Full implementation in `target/sparc/cpu.c` for SPARC/SunOS
- 🚧 **In Progress**: Migrating to generic `system/cpu-idle.c`
- 📋 **Planned**: Support for all architectures (x86, ARM, 6502, etc.)

## Files Created
1. `include/system/cpu-idle.h` - Public API and architecture interface
2. `system/cpu-idle.c` - Generic implementation framework (stubs)

## Migration Strategy

### Phase 1: Foundation (CURRENT)
- [x] Create `cpu-idle.h` with architecture-agnostic API
- [x] Create `cpu-idle.c` with basic structures
- [ ] Add to `system/meson.build`
- [ ] Ensure it compiles (even with stubs)

### Phase 2: Core Logic Migration
Extract from `target/sparc/cpu.c` lines 44-1700 (approximately):

#### Data Structures (DONE in cpu-idle.c)
- `LearningMode` enum
- `PCCandidate` struct
- `PCCollection` struct
- `collections[]` array
- Configuration variables

#### Helper Functions (TODO)
From `target/sparc/cpu.c`:
- Lines 1215-1224: `fopen_with_tmp_fallback()` ✅ DONE
- Lines 1227-1238: `stat_with_tmp_fallback()` ✅ DONE
- Lines 1240-1293: `recalculate_effective_counts()` 📋 TODO
- Lines 1296-1305: `compare_pc_candidates()` 📋 TODO
- Lines 1308-1364: `save_learned_pcs()` 📋 TODO
- Lines 1368-1455: `load_learned_pcs()` 📋 TODO

#### Main Detection Logic (TODO)
- Lines 874-1206: `sparc_cpu_exec_enter_hook()` → `cpu_idle_exec_hook()`
  - Speed measurement
  - Learning mode handling
  - Idle detection
  - Sleep calculation
  - Statistics reporting

#### Learning Control (TODO)
- Lines 1471-1479: `sparc_cpu_start_learning()` → `cpu_idle_start_*_learning()`
- Lines 1480-1585: `sparc_cpu_stop_learning()` → `cpu_idle_stop_learning()`

### Phase 3: Architecture Integration

#### SPARC (target/sparc/cpu.c)
```c
#include "system/cpu-idle.h"

// Implement architecture interface
void cpu_get_pc_state(CPUState *cpu, CPUPCState *state)
{
    CPUSPARCState *env = cpu_env(cpu);
    state->pc = env->pc;
    state->next_pc = env->npc;
}

// Register hardcoded fallback PCs
static target_ulong prom_idle_pcs[] = {
    0xffd16750, 0xffd20170, 0xffd20174, 0xffd2ba10, 0xffef0000
};

void sparc_cpu_initfn(Object *obj)
{
    // ... existing code ...

    cpu_idle_register_fallback_pcs(prom_idle_pcs,
                                   ARRAY_SIZE(prom_idle_pcs),
                                   0xf01294f8);  // SunOS idle PC
    cpu_idle_init();
}

// Replace exec_enter_hook
static void sparc_cpu_exec_enter_hook(CPUState *cs, TranslationBlock *tb)
{
    cpu_idle_exec_hook(cs);  // Call generic implementation
}
```

#### Future: 6502 (target/mos6502/cpu.c - example)
```c
#include "system/cpu-idle.h"

void cpu_get_pc_state(CPUState *cpu, CPUPCState *state)
{
    CPUMOS6502State *env = cpu_env(cpu);
    state->pc = env->pc;
    state->next_pc = env->pc + 1;  // 6502 has no explicit next_pc
}

// Apple II monitor wait loop, etc.
static target_ulong monitor_idle_pcs[] = {
    0xFD35,  // Example: Apple II monitor wait
};

void mos6502_cpu_initfn(Object *obj)
{
    cpu_idle_register_fallback_pcs(monitor_idle_pcs,
                                   ARRAY_SIZE(monitor_idle_pcs),
                                   0xFD35);
    cpu_idle_init();
}
```

### Phase 4: HMP Commands (TODO)
Rename in `hmp-commands.hx`:
- `sparc-cpu-debug` → `cpu-idle-debug`
- `sparc-start-prom-learning` → `cpu-idle-start-prom-learning`
- `sparc-start-idle-learning` → `cpu-idle-start-os-learning`
- `sparc-start-busy-learning` → `cpu-idle-start-busy-learning`
- `sparc-stop-learning` → `cpu-idle-stop-learning`

Update handlers in `system/cpu-idle.c`:
- `hmp_cpu_idle_debug()`
- `hmp_cpu_idle_start_prom_learning()`
- `hmp_cpu_idle_start_os_learning()`
- `hmp_cpu_idle_start_busy_learning()`
- `hmp_cpu_idle_stop_learning()`

### Phase 5: Testing
1. Test SPARC with refactored code
2. Verify all learning modes work
3. Confirm power-saving still functional
4. Check saved files use correct arch name

### Phase 6: Documentation
- Update QEMU documentation
- Add arch-specific examples
- Document HMP commands

## Benefits of Refactoring

### For Users
- ✅ Works on **all architectures** automatically
- ✅ Consistent HMP commands across architectures
- ✅ Power savings on any guest OS

### For Developers
- ✅ Single implementation to maintain
- ✅ Easy to add new architecture support (one function!)
- ✅ Better code organization

### For Emulation Community
- ✅ Retro computing (6502, Z80, etc.) gets power-saving
- ✅ ARM/x86 servers benefit too
- ✅ Universal performance tool

## Current File Sizes
- `target/sparc/cpu.c`: ~1951 lines (will shrink to ~800 after migration)
- `system/cpu-idle.c`: ~200 lines (will grow to ~1400)
- Net result: Better code organization, no duplication

## Timeline
- **Immediate**: Get stubs compiling
- **Short term**: Migrate core logic (1-2 days)
- **Medium term**: Full SPARC refactoring (1 week)
- **Long term**: Add other architectures (ongoing)

## Notes
- Keep SPARC working throughout migration
- Test at each step
- Commit frequently
- Document architecture interface thoroughly

