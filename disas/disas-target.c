/*
 * Routines for target instruction disassembly.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "disas/disas.h"
#include "disas/capstone.h"
#include "exec/translator.h"
#include "disas-internal.h"


static int translator_read_memory(bfd_vma memaddr, bfd_byte *myaddr,
                                  int length, struct disassemble_info *info)
{
    const DisasContextBase *db = info->application_data;
    return translator_st(db, myaddr, memaddr, length) ? 0 : EIO;
}

void target_disas(FILE *out, CPUState *cpu, const struct DisasContextBase *db, int *erm)
{
	//qemu_log("%s: Enter\n", __func__);
    uint64_t code = db->pc_first;
    size_t size = translator_st_len(db);
    uint64_t pc;
    int count;
    CPUDebug s;

    disas_initialize_debug_target(&s, cpu);
    s.info.read_memory_func = translator_read_memory;
    s.info.application_data = (void *)db;
    s.info.fprintf_func = fprintf;
    s.info.stream = out;
    s.info.buffer_vma = code;
    s.info.buffer_length = size;
    s.info.show_opcodes = true;

    if (s.info.cap_arch >= 0 && cap_disas_target(&s.info, code, size)) {
        return;
    }

    if (s.info.print_insn == NULL) {
        s.info.print_insn = print_insn_od_target;
    }

    for (pc = code; size > 0; pc += count, size -= count) {
        fprintf(out, "0x%08" PRIx64 ":  ", pc);
        count = s.info.print_insn(pc, &s.info); // HERE
		if (strcmp(s.info.opcode, "unimp")==0) *erm=1;
        fprintf(out, "\ncount=%d,size=%lu,insn_type=%d,data_size=%d,target=%ld,num_symbols=%d,opcode=%s\n",
				/*",cpu.crash=%d,cpu.exit=%d,cpu.dirty=%d.cpu.exception_index=%d,cpu.halted=%d"
				",cpu.ignore_mem=%d,cpu.throttle=%dcpu.index=%d\n",*/
				count,size,s.info.insn_type,s.info.data_size,s.info.target,s.info.num_symbols,s.info.opcode);
				/*cpu->crash_occurred,cpu->exit_request,cpu->dirty_pages,cpu->exception_index,cpu->halted,
				cpu->ignore_memory_transaction_failures,cpu->throttle_us_per_full.cpu->cpu_index);*/
		// TODO - break if erm?
        if (count < 0) {
            break;
        }
        if (size < count) {
            fprintf(out,
                    "Disassembler disagrees with translator over instruction "
                    "decoding\n"
                    "Please report this to qemu-devel@nongnu.org\n");
            break;
        }
    }
	//qemu_log("%s: Exit\n", __func__);
}

#ifdef CONFIG_PLUGIN
static void plugin_print_address(bfd_vma addr, struct disassemble_info *info)
{
    /* does nothing */
}

/*
 * We should only be dissembling one instruction at a time here. If
 * there is left over it usually indicates the front end has read more
 * bytes than it needed.
 */
char *plugin_disas(CPUState *cpu, const DisasContextBase *db,
                   uint64_t addr, size_t size)
{
    CPUDebug s;
    GString *ds = g_string_new(NULL);

    disas_initialize_debug_target(&s, cpu);
    s.info.read_memory_func = translator_read_memory;
    s.info.application_data = (void *)db;
    s.info.fprintf_func = disas_gstring_printf;
    s.info.stream = (FILE *)ds;  /* abuse this slot */
    s.info.buffer_vma = addr;
    s.info.buffer_length = size;
    s.info.print_address_func = plugin_print_address;

    if (s.info.cap_arch >= 0 && cap_disas_plugin(&s.info, addr, size)) {
        ; /* done */
    } else if (s.info.print_insn) {
        s.info.print_insn(addr, &s.info);
    } else {
        ; /* cannot disassemble -- return empty string */
    }

    /* Return the buffer, freeing the GString container.  */
    return g_string_free(ds, false);
}
#endif /* CONFIG_PLUGIN */
