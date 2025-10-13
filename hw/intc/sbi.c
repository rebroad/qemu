/*
 * QEMU Sparc SBI interrupt controller emulation
 *
 * Based on slavio_intctl, copyright (c) 2003-2005 Fabrice Bellard
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

#include "qemu/osdep.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "hw/sysbus.h"
#include "hw/intc/intc.h"
#include "hw/irq.h"
#include "trace.h"
#include "qom/object.h"

//#define DEBUG_IRQ

#ifdef DEBUG_IRQ
#define DPRINTF(fmt, ...)                                       \
    do { printf("IRQ: " fmt , ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...)
#endif

#define MAX_CPUS 16

#define SBI_NREGS 16

#define TYPE_SBI "sbi"
OBJECT_DECLARE_SIMPLE_TYPE(SBIState, SBI)

struct SBIState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[SBI_NREGS];
    uint32_t intreg_pending[MAX_CPUS];
    qemu_irq cpu_irqs[MAX_CPUS];
    uint32_t pil_out[MAX_CPUS];
};

#define SBI_SIZE (SBI_NREGS * 4)

static void sbi_set_irq(void *opaque, int irq, int level)
{
}

static uint64_t sbi_mem_read(void *opaque, hwaddr addr,
                             unsigned size)
{
    SBIState *s = opaque;
    uint32_t saddr, ret;

    saddr = addr >> 2;
    switch (saddr) {
    default:
        ret = s->regs[saddr];
        break;
    }
    DPRINTF("read system reg 0x" TARGET_FMT_plx " = %x\n", addr, ret);

    return ret;
}

static void sbi_mem_write(void *opaque, hwaddr addr,
                          uint64_t val, unsigned dize)
{
    SBIState *s = opaque;
    uint32_t saddr;

    saddr = addr >> 2;
    DPRINTF("write system reg 0x" TARGET_FMT_plx " = %x\n", addr, (int)val);
    switch (saddr) {
    default:
        s->regs[saddr] = val;
        break;
    }
}

static const MemoryRegionOps sbi_mem_ops = {
    .read = sbi_mem_read,
    .write = sbi_mem_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static const VMStateDescription vmstate_sbi = {
    .name ="sbi",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields      = (VMStateField []) {
        VMSTATE_UINT32_ARRAY(intreg_pending, SBIState, MAX_CPUS),
        VMSTATE_END_OF_LIST()
    }
};

static void sbi_reset(DeviceState *d)
{
    SBIState *s = SBI(d);
    unsigned int i;

    for (i = 0; i < MAX_CPUS; i++) {
        s->intreg_pending[i] = 0;
    }
}

static void sbi_init(Object *obj)
{
    SBIState *s = SBI(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DeviceState *dev = DEVICE(obj);
    unsigned int i;

    qdev_init_gpio_in(dev, sbi_set_irq, 32 + MAX_CPUS);
    for (i = 0; i < MAX_CPUS; i++) {
        sysbus_init_irq(sbd, &s->cpu_irqs[i]);
    }

    memory_region_init_io(&s->iomem, obj, &sbi_mem_ops, s, "sbi", SBI_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void sbi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, sbi_reset);
    dc->vmsd = &vmstate_sbi;
}

static const TypeInfo sbi_info = {
    .name          = TYPE_SBI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SBIState),
    .instance_init = sbi_init,
    .class_init    = sbi_class_init,
};

static void sbi_register_types(void)
{
    type_register_static(&sbi_info);
}

type_init(sbi_register_types)
