/*
 * RP2040 single-cycle IO block emulation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/rp2040_sio.h"
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
#define SIO_SPINLOCK_BASE       0x100
#define SIO_SPINLOCK_LAST       0x17c

#define SIO_GPIO_MASK           0x3fffffff
#define SIO_GPIO_HI_MASK        0x3f
#define SIO_FIFO_ST_RDY         BIT(1)
#define SIO_FIFO_ST_WC_MASK     (BIT(3) | BIT(2))

static bool rp2040_sio_spinlock_offset(hwaddr offset, unsigned *index)
{
    if (offset < SIO_SPINLOCK_BASE || offset > SIO_SPINLOCK_LAST ||
        (offset & 0x3)) {
        return false;
    }

    *index = (offset - SIO_SPINLOCK_BASE) / sizeof(uint32_t);
    return *index < 32;
}

static uint64_t rp2040_sio_read(void *opaque, hwaddr addr, unsigned size)
{
    RP2040SioState *s = opaque;
    unsigned index;
    uint64_t value;

    switch (addr) {
    case SIO_CPUID:
        value = 0;
        break;
    case SIO_GPIO_IN:
    case SIO_GPIO_HI_IN:
        value = 0;
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
        value = SIO_FIFO_ST_RDY | s->fifo_sticky;
        break;
    case SIO_FIFO_RD:
        s->fifo_sticky |= BIT(3);
        value = 0;
        break;
    case SIO_SPINLOCK_ST:
        value = s->spinlock_st;
        break;
    default:
        if (rp2040_sio_spinlock_offset(addr, &index)) {
            value = (s->spinlock_st & BIT(index)) ? 0 : BIT(index);
            s->spinlock_st |= BIT(index);
        } else {
            value = 0;
        }
        break;
    }

    qemu_log_mask(LOG_UNIMP, "rp2040.sio: read  "
                  "(size %d, addr 0x%08" HWADDR_PRIx
                  ", offset 0x%04" HWADDR_PRIx ") -> 0x%0*" PRIx64 "\n",
                  size, RP2040_SIO_BASE + addr, addr, size << 1, value);
    return value;
}

static void rp2040_sio_write(void *opaque, hwaddr addr,
                             uint64_t value64, unsigned size)
{
    RP2040SioState *s = opaque;
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
        s->fifo_sticky &= ~(value & SIO_FIFO_ST_WC_MASK);
        break;
    case SIO_FIFO_WR:
        break;
    default:
        if (rp2040_sio_spinlock_offset(addr, &index)) {
            s->spinlock_st &= ~BIT(index);
        }
        break;
    }

    qemu_log_mask(LOG_UNIMP, "rp2040.sio: write "
                  "(size %d, addr 0x%08" HWADDR_PRIx
                  ", offset 0x%04" HWADDR_PRIx
                  ", value 0x%0*" PRIx64 ")\n",
                  size, RP2040_SIO_BASE + addr, addr, size << 1, value64);
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

    s->gpio_out = 0;
    s->gpio_oe = 0;
    s->gpio_hi_out = 0;
    s->gpio_hi_oe = 0;
    s->fifo_sticky = 0;
    s->spinlock_st = 0;
}

static void rp2040_sio_init(Object *obj)
{
    RP2040SioState *s = RP2040_SIO(obj);

    memory_region_init_io(&s->iomem, obj, &rp2040_sio_ops, s,
                          TYPE_RP2040_SIO, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_rp2040_sio = {
    .name = TYPE_RP2040_SIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(gpio_out, RP2040SioState),
        VMSTATE_UINT32(gpio_oe, RP2040SioState),
        VMSTATE_UINT32(gpio_hi_out, RP2040SioState),
        VMSTATE_UINT32(gpio_hi_oe, RP2040SioState),
        VMSTATE_UINT32(fifo_sticky, RP2040SioState),
        VMSTATE_UINT32(spinlock_st, RP2040SioState),
        VMSTATE_END_OF_LIST()
    }
};

static void rp2040_sio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, rp2040_sio_reset);
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
