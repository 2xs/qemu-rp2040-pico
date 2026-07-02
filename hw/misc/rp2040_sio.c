/*
 * RP2040 single-cycle IO block emulation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/rp2040_nyi.h"
#include "hw/misc/rp2040_sio.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define SIO_CPUID               0x000
#define SIO_GPIO_IN             0x004
#define SIO_GPIO_HI_IN          0x008
#define SIO_GPIO_OUT            0x010
#define SIO_GPIO_OUT_SET        0x014
#define SIO_GPIO_OUT_CLR        0x018
#define SIO_GPIO_OUT_XOR        0x01c
#define SIO_GPIO_OE             0x020
#define SIO_GPIO_OE_SET         0x024
#define SIO_GPIO_OE_CLR         0x028
#define SIO_GPIO_OE_XOR         0x02c
#define SIO_GPIO_HI_OUT         0x030
#define SIO_GPIO_HI_OUT_SET     0x034
#define SIO_GPIO_HI_OUT_CLR     0x038
#define SIO_GPIO_HI_OUT_XOR     0x03c
#define SIO_GPIO_HI_OE          0x040
#define SIO_GPIO_HI_OE_SET      0x044
#define SIO_GPIO_HI_OE_CLR      0x048
#define SIO_GPIO_HI_OE_XOR      0x04c
#define SIO_FIFO_ST             0x050
#define SIO_FIFO_WR             0x054
#define SIO_FIFO_RD             0x058
#define SIO_SPINLOCK_ST         0x05c
#define SIO_DIV_UDIVIDEND       0x060
#define SIO_DIV_UDIVISOR        0x064
#define SIO_DIV_SDIVIDEND       0x068
#define SIO_DIV_SDIVISOR        0x06c
#define SIO_DIV_QUOTIENT        0x070
#define SIO_DIV_REMAINDER       0x074
#define SIO_DIV_CSR             0x078
#define SIO_SPINLOCK_BASE       0x100
#define SIO_SPINLOCK_LAST       0x17c

#define SIO_GPIO_MASK           0x3fffffff
#define SIO_GPIO_HI_MASK        0x3f
#define SIO_FIFO_ST_VLD         BIT(0)
#define SIO_FIFO_ST_RDY         BIT(1)
#define SIO_FIFO_ST_WC_MASK     (BIT(3) | BIT(2))
#define SIO_DIV_CSR_READY       BIT(0)
#define SIO_DIV_CSR_DIRTY       BIT(1)

static unsigned rp2040_sio_current_core(void)
{
    /*
     * current_cpu is QEMU's thread-local pointer to the currently executing
     * guest vCPU, not a host CPU identifier.
     */
    if (current_cpu && current_cpu->cpu_index == 1) {
        return 1;
    }

    return 0;
}

static void rp2040_sio_update_fifo_irq(RP2040SioState *s)
{
    int i;

    for (i = 0; i < RP2040_SIO_NUM_CORES; i++) {
        qemu_set_irq(s->fifo_irq[i], s->fifo_level[i] != 0);
    }
}

static uint32_t rp2040_sio_fifo_status(RP2040SioState *s, unsigned core)
{
    unsigned peer = core ^ 1;
    uint32_t value = s->fifo_sticky[core] & SIO_FIFO_ST_WC_MASK;

    if (s->fifo_level[core] != 0) {
        value |= SIO_FIFO_ST_VLD;
    }
    if (s->fifo_level[peer] < RP2040_SIO_FIFO_DEPTH) {
        value |= SIO_FIFO_ST_RDY;
    }

    return value;
}

static void rp2040_sio_fifo_push(RP2040SioState *s, unsigned core,
                                 uint32_t value)
{
    unsigned peer = core ^ 1;

    if (s->fifo_level[peer] == RP2040_SIO_FIFO_DEPTH) {
        s->fifo_sticky[core] |= BIT(2);
        return;
    }

    s->fifo[peer][s->fifo_wptr[peer]] = value;
    s->fifo_wptr[peer] = (s->fifo_wptr[peer] + 1) % RP2040_SIO_FIFO_DEPTH;
    s->fifo_level[peer]++;
    rp2040_sio_update_fifo_irq(s);
}

static uint32_t rp2040_sio_fifo_pop(RP2040SioState *s, unsigned core)
{
    uint32_t value;

    if (s->fifo_level[core] == 0) {
        s->fifo_sticky[core] |= BIT(3);
        return 0;
    }

    value = s->fifo[core][s->fifo_rptr[core]];
    s->fifo_rptr[core] = (s->fifo_rptr[core] + 1) % RP2040_SIO_FIFO_DEPTH;
    s->fifo_level[core]--;
    rp2040_sio_update_fifo_irq(s);

    return value;
}

static bool rp2040_sio_spinlock_offset(hwaddr offset, unsigned *index)
{
    if (offset < SIO_SPINLOCK_BASE || offset > SIO_SPINLOCK_LAST ||
        (offset & 0x3)) {
        return false;
    }

    *index = (offset - SIO_SPINLOCK_BASE) / sizeof(uint32_t);
    return *index < 32;
}

static int32_t rp2040_sio_sign32(uint32_t value)
{
    int32_t signed_value = (int32_t)value;

    if (signed_value > 0) {
        return 1;
    }
    if (signed_value < 0) {
        return -1;
    }
    return 0;
}

static void rp2040_sio_divide(RP2040SioState *s, unsigned core,
                              bool is_signed)
{
    uint32_t dividend = s->div_dividend[core];
    uint32_t divisor = s->div_divisor[core];
    uint32_t quotient;
    uint32_t remainder;

    if (is_signed) {
        int32_t signed_dividend = (int32_t)dividend;
        int32_t signed_divisor = (int32_t)divisor;

        if (signed_divisor == 0) {
            quotient = -rp2040_sio_sign32(dividend);
            remainder = dividend;
        } else if (signed_dividend == INT32_MIN && signed_divisor == -1) {
            quotient = dividend;
            remainder = 0;
        } else {
            quotient = signed_dividend / signed_divisor;
            remainder = signed_dividend % signed_divisor;
        }
    } else if (divisor == 0) {
        quotient = UINT32_MAX;
        remainder = dividend;
    } else {
        quotient = dividend / divisor;
        remainder = dividend % divisor;
    }

    s->div_quotient[core] = quotient;
    s->div_remainder[core] = remainder;
    s->div_dirty[core] = true;
}

static uint32_t rp2040_sio_div_csr(RP2040SioState *s, unsigned core)
{
    return SIO_DIV_CSR_READY |
           (s->div_dirty[core] ? SIO_DIV_CSR_DIRTY : 0);
}

static uint64_t rp2040_sio_read(void *opaque, hwaddr addr, unsigned size)
{
    RP2040SioState *s = opaque;
    unsigned core = rp2040_sio_current_core();
    unsigned index;
    uint64_t value;

    switch (addr) {
    case SIO_CPUID:
        value = core;
        break;
    case SIO_GPIO_IN:
        value = s->gpio_in;
        break;
    case SIO_GPIO_HI_IN:
        value = s->gpio_hi_in;
        break;
    case SIO_GPIO_OUT:
        value = s->gpio_out;
        break;
    case SIO_GPIO_OE:
        value = s->gpio_oe;
        break;
    case SIO_GPIO_HI_OUT:
        value = s->gpio_hi_out;
        break;
    case SIO_GPIO_HI_OE:
        value = s->gpio_hi_oe;
        break;
    case SIO_FIFO_ST:
        value = rp2040_sio_fifo_status(s, core);
        break;
    case SIO_FIFO_RD:
        value = rp2040_sio_fifo_pop(s, core);
        break;
    case SIO_SPINLOCK_ST:
        value = s->spinlock_st;
        break;
    case SIO_DIV_UDIVIDEND:
    case SIO_DIV_SDIVIDEND:
        value = s->div_dividend[core];
        break;
    case SIO_DIV_UDIVISOR:
    case SIO_DIV_SDIVISOR:
        value = s->div_divisor[core];
        break;
    case SIO_DIV_QUOTIENT:
        value = s->div_quotient[core];
        s->div_dirty[core] = false;
        break;
    case SIO_DIV_REMAINDER:
        value = s->div_remainder[core];
        break;
    case SIO_DIV_CSR:
        value = rp2040_sio_div_csr(s, core);
        break;
    default:
        if (rp2040_sio_spinlock_offset(addr, &index)) {
            value = (s->spinlock_st & BIT(index)) ? 0 : BIT(index);
            s->spinlock_st |= BIT(index);
        } else {
            value = 0;
            rp2040_log_unimplemented_read("sio", size,
                                          RP2040_SIO_BASE + addr, addr,
                                          value);
        }
        break;
    }

    return value;
}

static void rp2040_sio_write(void *opaque, hwaddr addr,
                             uint64_t value64, unsigned size)
{
    RP2040SioState *s = opaque;
    unsigned core = rp2040_sio_current_core();
    unsigned index;
    uint32_t value = value64;

    switch (addr) {
    case SIO_GPIO_OUT:
        s->gpio_out = value & SIO_GPIO_MASK;
        break;
    case SIO_GPIO_OUT_SET:
        s->gpio_out |= value & SIO_GPIO_MASK;
        break;
    case SIO_GPIO_OUT_CLR:
        s->gpio_out &= ~(value & SIO_GPIO_MASK);
        break;
    case SIO_GPIO_OUT_XOR:
        s->gpio_out ^= value & SIO_GPIO_MASK;
        break;
    case SIO_GPIO_OE:
        s->gpio_oe = value & SIO_GPIO_MASK;
        break;
    case SIO_GPIO_OE_SET:
        s->gpio_oe |= value & SIO_GPIO_MASK;
        break;
    case SIO_GPIO_OE_CLR:
        s->gpio_oe &= ~(value & SIO_GPIO_MASK);
        break;
    case SIO_GPIO_OE_XOR:
        s->gpio_oe ^= value & SIO_GPIO_MASK;
        break;
    case SIO_GPIO_HI_OUT:
        s->gpio_hi_out = value & SIO_GPIO_HI_MASK;
        break;
    case SIO_GPIO_HI_OUT_SET:
        s->gpio_hi_out |= value & SIO_GPIO_HI_MASK;
        break;
    case SIO_GPIO_HI_OUT_CLR:
        s->gpio_hi_out &= ~(value & SIO_GPIO_HI_MASK);
        break;
    case SIO_GPIO_HI_OUT_XOR:
        s->gpio_hi_out ^= value & SIO_GPIO_HI_MASK;
        break;
    case SIO_GPIO_HI_OE:
        s->gpio_hi_oe = value & SIO_GPIO_HI_MASK;
        break;
    case SIO_GPIO_HI_OE_SET:
        s->gpio_hi_oe |= value & SIO_GPIO_HI_MASK;
        break;
    case SIO_GPIO_HI_OE_CLR:
        s->gpio_hi_oe &= ~(value & SIO_GPIO_HI_MASK);
        break;
    case SIO_GPIO_HI_OE_XOR:
        s->gpio_hi_oe ^= value & SIO_GPIO_HI_MASK;
        break;
    case SIO_FIFO_ST:
        s->fifo_sticky[core] &= ~(value & SIO_FIFO_ST_WC_MASK);
        break;
    case SIO_FIFO_WR:
        rp2040_sio_fifo_push(s, core, value);
        break;
    case SIO_DIV_UDIVIDEND:
        s->div_dividend[core] = value;
        rp2040_sio_divide(s, core, false);
        break;
    case SIO_DIV_UDIVISOR:
        s->div_divisor[core] = value;
        rp2040_sio_divide(s, core, false);
        break;
    case SIO_DIV_SDIVIDEND:
        s->div_dividend[core] = value;
        rp2040_sio_divide(s, core, true);
        break;
    case SIO_DIV_SDIVISOR:
        s->div_divisor[core] = value;
        rp2040_sio_divide(s, core, true);
        break;
    case SIO_DIV_QUOTIENT:
        s->div_quotient[core] = value;
        s->div_dirty[core] = true;
        break;
    case SIO_DIV_REMAINDER:
        s->div_remainder[core] = value;
        s->div_dirty[core] = true;
        break;
    default:
        if (rp2040_sio_spinlock_offset(addr, &index)) {
            s->spinlock_st &= ~BIT(index);
        } else {
            rp2040_log_unimplemented_write("sio", size,
                                           RP2040_SIO_BASE + addr, addr,
                                           value64);
        }
        break;
    }
}

static const MemoryRegionOps rp2040_sio_ops = {
    .read = rp2040_sio_read,
    .write = rp2040_sio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void rp2040_sio_reset(DeviceState *dev)
{
    RP2040SioState *s = RP2040_SIO(dev);

    s->gpio_in &= SIO_GPIO_MASK;
    s->gpio_hi_in &= SIO_GPIO_HI_MASK;
    s->gpio_out = 0;
    s->gpio_oe = 0;
    s->gpio_hi_out = 0;
    s->gpio_hi_oe = 0;
    memset(s->fifo, 0, sizeof(s->fifo));
    memset(s->fifo_rptr, 0, sizeof(s->fifo_rptr));
    memset(s->fifo_wptr, 0, sizeof(s->fifo_wptr));
    memset(s->fifo_level, 0, sizeof(s->fifo_level));
    memset(s->fifo_sticky, 0, sizeof(s->fifo_sticky));
    memset(s->div_dividend, 0, sizeof(s->div_dividend));
    memset(s->div_divisor, 0, sizeof(s->div_divisor));
    memset(s->div_quotient, 0, sizeof(s->div_quotient));
    memset(s->div_remainder, 0, sizeof(s->div_remainder));
    memset(s->div_dirty, 0, sizeof(s->div_dirty));
    s->spinlock_st = 0;
    rp2040_sio_update_fifo_irq(s);
}

static void rp2040_sio_init(Object *obj)
{
    RP2040SioState *s = RP2040_SIO(obj);

    memory_region_init_io(&s->iomem, obj, &rp2040_sio_ops, s,
                          TYPE_RP2040_SIO, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->fifo_irq[0]);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->fifo_irq[1]);
}

static const VMStateDescription vmstate_rp2040_sio = {
    .name = TYPE_RP2040_SIO,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(gpio_out, RP2040SioState),
        VMSTATE_UINT32(gpio_in, RP2040SioState),
        VMSTATE_UINT32(gpio_hi_in, RP2040SioState),
        VMSTATE_UINT32(gpio_oe, RP2040SioState),
        VMSTATE_UINT32(gpio_hi_out, RP2040SioState),
        VMSTATE_UINT32(gpio_hi_oe, RP2040SioState),
        VMSTATE_UINT32_2DARRAY(fifo, RP2040SioState, RP2040_SIO_NUM_CORES,
                               RP2040_SIO_FIFO_DEPTH),
        VMSTATE_UINT8_ARRAY(fifo_rptr, RP2040SioState,
                            RP2040_SIO_NUM_CORES),
        VMSTATE_UINT8_ARRAY(fifo_wptr, RP2040SioState,
                            RP2040_SIO_NUM_CORES),
        VMSTATE_UINT8_ARRAY(fifo_level, RP2040SioState,
                            RP2040_SIO_NUM_CORES),
        VMSTATE_UINT32_ARRAY(fifo_sticky, RP2040SioState,
                             RP2040_SIO_NUM_CORES),
        VMSTATE_UINT32_ARRAY(div_dividend, RP2040SioState,
                             RP2040_SIO_NUM_CORES),
        VMSTATE_UINT32_ARRAY(div_divisor, RP2040SioState,
                             RP2040_SIO_NUM_CORES),
        VMSTATE_UINT32_ARRAY(div_quotient, RP2040SioState,
                             RP2040_SIO_NUM_CORES),
        VMSTATE_UINT32_ARRAY(div_remainder, RP2040SioState,
                             RP2040_SIO_NUM_CORES),
        VMSTATE_BOOL_ARRAY(div_dirty, RP2040SioState,
                           RP2040_SIO_NUM_CORES),
        VMSTATE_UINT32(spinlock_st, RP2040SioState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property rp2040_sio_properties[] = {
    DEFINE_PROP_UINT32("gpio-in", RP2040SioState, gpio_in, 0),
    DEFINE_PROP_UINT32("gpio-hi-in", RP2040SioState, gpio_hi_in, 0),
};

static void rp2040_sio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, rp2040_sio_reset);
    device_class_set_props(dc, rp2040_sio_properties);
    dc->vmsd = &vmstate_rp2040_sio;
}

static const TypeInfo rp2040_sio_info = {
    .name = TYPE_RP2040_SIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RP2040SioState),
    .instance_init = rp2040_sio_init,
    .class_init = rp2040_sio_class_init,
};

static void rp2040_sio_register_types(void)
{
    type_register_static(&rp2040_sio_info);
}
type_init(rp2040_sio_register_types)
