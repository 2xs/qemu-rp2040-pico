/*
 * RP2040 DMA emulation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/dma/rp2040_dma.h"
#include "hw/misc/rp2040_nyi.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define DMA_CH_SIZE              0x40
#define DMA_CH_READ_ADDR         0x00
#define DMA_CH_WRITE_ADDR        0x04
#define DMA_CH_TRANS_COUNT       0x08
#define DMA_CH_CTRL_TRIG         0x0c
#define DMA_CH_AL1_CTRL          0x10
#define DMA_CH_AL1_READ_ADDR     0x14
#define DMA_CH_AL1_WRITE_ADDR    0x18
#define DMA_CH_AL1_TRANS_COUNT   0x1c
#define DMA_CH_AL2_CTRL          0x20
#define DMA_CH_AL2_TRANS_COUNT   0x24
#define DMA_CH_AL2_READ_ADDR     0x28
#define DMA_CH_AL2_WRITE_ADDR    0x2c
#define DMA_CH_AL3_CTRL          0x30
#define DMA_CH_AL3_WRITE_ADDR    0x34
#define DMA_CH_AL3_TRANS_COUNT   0x38
#define DMA_CH_AL3_READ_ADDR     0x3c

#define DMA_INTR                 0x400
#define DMA_INTE0                0x404
#define DMA_INTF0                0x408
#define DMA_INTS0                0x40c
#define DMA_INTE1                0x414
#define DMA_INTF1                0x418
#define DMA_INTS1                0x41c
#define DMA_TIMER0               0x420
#define DMA_MULTI_CHAN_TRIGGER   0x430
#define DMA_SNIFF_CTRL           0x434
#define DMA_SNIFF_DATA           0x438
#define DMA_FIFO_LEVELS          0x440
#define DMA_CHAN_ABORT           0x444

#define DMA_CTRL_AHB_ERROR       BIT(31)
#define DMA_CTRL_READ_ERROR      BIT(30)
#define DMA_CTRL_WRITE_ERROR     BIT(29)
#define DMA_CTRL_BUSY            BIT(24)
#define DMA_CTRL_SNIFF_EN        BIT(23)
#define DMA_CTRL_BSWAP           BIT(22)
#define DMA_CTRL_IRQ_QUIET       BIT(21)
#define DMA_CTRL_TREQ_SEL_SHIFT  15
#define DMA_CTRL_TREQ_SEL_MASK   (0x3f << DMA_CTRL_TREQ_SEL_SHIFT)
#define DMA_CTRL_CHAIN_TO_SHIFT  11
#define DMA_CTRL_CHAIN_TO_MASK   (0xf << DMA_CTRL_CHAIN_TO_SHIFT)
#define DMA_CTRL_RING_SEL        BIT(10)
#define DMA_CTRL_RING_SIZE_MASK  (0xf << 6)
#define DMA_CTRL_INCR_WRITE      BIT(5)
#define DMA_CTRL_INCR_READ       BIT(4)
#define DMA_CTRL_DATA_SIZE_SHIFT 2
#define DMA_CTRL_DATA_SIZE_MASK  (0x3 << DMA_CTRL_DATA_SIZE_SHIFT)
#define DMA_CTRL_EN              BIT(0)
#define DMA_CTRL_ERROR_MASK      (DMA_CTRL_AHB_ERROR | \
                                  DMA_CTRL_READ_ERROR | \
                                  DMA_CTRL_WRITE_ERROR)

#define DMA_CTRL_WRITABLE_MASK   (DMA_CTRL_SNIFF_EN | DMA_CTRL_BSWAP | \
                                  DMA_CTRL_IRQ_QUIET | \
                                  DMA_CTRL_TREQ_SEL_MASK | \
                                  DMA_CTRL_CHAIN_TO_MASK | \
                                  DMA_CTRL_RING_SEL | \
                                  DMA_CTRL_RING_SIZE_MASK | \
                                  DMA_CTRL_INCR_WRITE | \
                                  DMA_CTRL_INCR_READ | \
                                  DMA_CTRL_DATA_SIZE_MASK | \
                                  BIT(1) | DMA_CTRL_EN)
#define DMA_TREQ_FORCE           0x3f
#define DMA_CHANNEL_MASK         ((1u << RP2040_DMA_NUM_CHANNELS) - 1)

#define ATOMIC_ALIAS_MASK        0x3000
#define ATOMIC_XOR               0x1000
#define ATOMIC_SET               0x2000
#define ATOMIC_CLR               0x3000

static uint32_t rp2040_dma_apply_alias(uint32_t old, uint32_t value,
                                       hwaddr alias)
{
    switch (alias) {
    case ATOMIC_XOR:
        return old ^ value;
    case ATOMIC_SET:
        return old | value;
    case ATOMIC_CLR:
        return old & ~value;
    default:
        return value;
    }
}

static uint32_t rp2040_dma_ints(RP2040DmaState *s, unsigned irq)
{
    return ((s->intr & s->inte[irq]) | s->intf[irq]) & DMA_CHANNEL_MASK;
}

static void rp2040_dma_update_irq(RP2040DmaState *s)
{
    int i;

    for (i = 0; i < RP2040_DMA_NUM_IRQS; i++) {
        qemu_set_irq(s->irq[i], rp2040_dma_ints(s, i) != 0);
    }
}

static unsigned rp2040_dma_transfer_size(RP2040DmaChannel *ch)
{
    switch ((ch->ctrl & DMA_CTRL_DATA_SIZE_MASK) >> DMA_CTRL_DATA_SIZE_SHIFT) {
    case 0:
        return 1;
    case 1:
        return 2;
    case 2:
        return 4;
    default:
        return 4;
    }
}

static void rp2040_dma_start_channel(RP2040DmaState *s, unsigned index,
                                     unsigned chain_depth)
{
    RP2040DmaChannel *ch = &s->chan[index];
    uint32_t chain_to;
    uint32_t count;
    unsigned width;

    if (!(ch->ctrl & DMA_CTRL_EN) || ch->trans_count == 0) {
        return;
    }

    if ((ch->ctrl & DMA_CTRL_TREQ_SEL_MASK) !=
        (DMA_TREQ_FORCE << DMA_CTRL_TREQ_SEL_SHIFT)) {
        rp2040_log_nyi("dma", "paced transfer",
                       "non-FORCE DREQ is treated as immediately ready");
    }
    if (ch->ctrl & (DMA_CTRL_SNIFF_EN | DMA_CTRL_RING_SIZE_MASK)) {
        rp2040_log_nyi("dma", "sniff/ring transfer",
                       "transfer runs without checksum or ring wrapping");
    }

    ch->ctrl |= DMA_CTRL_BUSY;
    ch->ctrl &= ~DMA_CTRL_ERROR_MASK;
    width = rp2040_dma_transfer_size(ch);
    count = ch->trans_count;

    while (count--) {
        uint8_t buf[4] = { 0 };
        MemTxResult result;

        result = address_space_rw(&s->dma_as, ch->read_addr,
                                  MEMTXATTRS_UNSPECIFIED, buf, width, false);
        if (result != MEMTX_OK) {
            ch->ctrl |= DMA_CTRL_READ_ERROR | DMA_CTRL_AHB_ERROR;
            break;
        }

        if (ch->ctrl & DMA_CTRL_BSWAP) {
            if (width == 2) {
                uint8_t t = buf[0];
                buf[0] = buf[1];
                buf[1] = t;
            } else if (width == 4) {
                uint8_t t = buf[0];
                buf[0] = buf[3];
                buf[3] = t;
                t = buf[1];
                buf[1] = buf[2];
                buf[2] = t;
            }
        }

        result = address_space_rw(&s->dma_as, ch->write_addr,
                                  MEMTXATTRS_UNSPECIFIED, buf, width, true);
        if (result != MEMTX_OK) {
            ch->ctrl |= DMA_CTRL_WRITE_ERROR | DMA_CTRL_AHB_ERROR;
            break;
        }

        ch->trans_count--;
        if (ch->ctrl & DMA_CTRL_INCR_READ) {
            ch->read_addr += width;
        }
        if (ch->ctrl & DMA_CTRL_INCR_WRITE) {
            ch->write_addr += width;
        }
    }

    ch->ctrl &= ~DMA_CTRL_BUSY;
    if (!(ch->ctrl & DMA_CTRL_ERROR_MASK) && !(ch->ctrl & DMA_CTRL_IRQ_QUIET)) {
        s->intr |= BIT(index);
        rp2040_dma_update_irq(s);
    }

    chain_to = (ch->ctrl & DMA_CTRL_CHAIN_TO_MASK) >> DMA_CTRL_CHAIN_TO_SHIFT;
    if (!(ch->ctrl & DMA_CTRL_ERROR_MASK) &&
        chain_to < RP2040_DMA_NUM_CHANNELS && chain_to != index &&
        chain_depth < RP2040_DMA_NUM_CHANNELS) {
        rp2040_dma_start_channel(s, chain_to, chain_depth + 1);
    }
}

static uint32_t rp2040_dma_read_channel(RP2040DmaState *s, unsigned index,
                                        hwaddr offset)
{
    RP2040DmaChannel *ch = &s->chan[index];

    switch (offset) {
    case DMA_CH_READ_ADDR:
    case DMA_CH_AL1_READ_ADDR:
    case DMA_CH_AL2_READ_ADDR:
    case DMA_CH_AL3_READ_ADDR:
        return ch->read_addr;
    case DMA_CH_WRITE_ADDR:
    case DMA_CH_AL1_WRITE_ADDR:
    case DMA_CH_AL2_WRITE_ADDR:
    case DMA_CH_AL3_WRITE_ADDR:
        return ch->write_addr;
    case DMA_CH_TRANS_COUNT:
    case DMA_CH_AL1_TRANS_COUNT:
    case DMA_CH_AL2_TRANS_COUNT:
    case DMA_CH_AL3_TRANS_COUNT:
        return ch->trans_count;
    case DMA_CH_CTRL_TRIG:
    case DMA_CH_AL1_CTRL:
    case DMA_CH_AL2_CTRL:
    case DMA_CH_AL3_CTRL:
        return ch->ctrl;
    default:
        return 0;
    }
}

static void rp2040_dma_write_ctrl(RP2040DmaState *s, unsigned index,
                                  uint32_t value, bool trigger)
{
    RP2040DmaChannel *ch = &s->chan[index];
    uint32_t errors = ch->ctrl & DMA_CTRL_ERROR_MASK;

    errors &= ~(value & DMA_CTRL_ERROR_MASK);
    ch->ctrl = (value & DMA_CTRL_WRITABLE_MASK) | errors;
    if (trigger) {
        rp2040_dma_start_channel(s, index, 0);
    }
}

static void rp2040_dma_write_channel(RP2040DmaState *s, unsigned index,
                                     hwaddr offset, uint32_t value)
{
    RP2040DmaChannel *ch = &s->chan[index];

    switch (offset) {
    case DMA_CH_READ_ADDR:
    case DMA_CH_AL1_READ_ADDR:
    case DMA_CH_AL2_READ_ADDR:
        ch->read_addr = value;
        break;
    case DMA_CH_AL3_READ_ADDR:
        ch->read_addr = value;
        rp2040_dma_start_channel(s, index, 0);
        break;
    case DMA_CH_WRITE_ADDR:
    case DMA_CH_AL1_WRITE_ADDR:
    case DMA_CH_AL3_WRITE_ADDR:
        ch->write_addr = value;
        break;
    case DMA_CH_AL2_WRITE_ADDR:
        ch->write_addr = value;
        rp2040_dma_start_channel(s, index, 0);
        break;
    case DMA_CH_TRANS_COUNT:
    case DMA_CH_AL2_TRANS_COUNT:
    case DMA_CH_AL3_TRANS_COUNT:
        ch->trans_count = value;
        ch->reload_count = value;
        break;
    case DMA_CH_AL1_TRANS_COUNT:
        ch->trans_count = value;
        ch->reload_count = value;
        rp2040_dma_start_channel(s, index, 0);
        break;
    case DMA_CH_CTRL_TRIG:
        rp2040_dma_write_ctrl(s, index, value, true);
        break;
    case DMA_CH_AL1_CTRL:
    case DMA_CH_AL2_CTRL:
    case DMA_CH_AL3_CTRL:
        rp2040_dma_write_ctrl(s, index, value, false);
        break;
    default:
        break;
    }
}

static uint64_t rp2040_dma_read(void *opaque, hwaddr addr, unsigned size)
{
    RP2040DmaState *s = opaque;
    hwaddr offset = addr & 0xfff;
    uint32_t value;

    if (offset < RP2040_DMA_NUM_CHANNELS * DMA_CH_SIZE) {
        value = rp2040_dma_read_channel(s, offset / DMA_CH_SIZE,
                                        offset % DMA_CH_SIZE);
    } else {
        switch (offset) {
        case DMA_INTR:
            value = s->intr;
            break;
        case DMA_INTE0:
            value = s->inte[0];
            break;
        case DMA_INTF0:
            value = s->intf[0];
            break;
        case DMA_INTS0:
            value = rp2040_dma_ints(s, 0);
            break;
        case DMA_INTE1:
            value = s->inte[1];
            break;
        case DMA_INTF1:
            value = s->intf[1];
            break;
        case DMA_INTS1:
            value = rp2040_dma_ints(s, 1);
            break;
        case DMA_TIMER0 ... DMA_TIMER0 + 3 * sizeof(uint32_t):
            value = s->timer[(offset - DMA_TIMER0) / sizeof(uint32_t)];
            break;
        case DMA_SNIFF_CTRL:
            value = s->sniff_ctrl;
            break;
        case DMA_SNIFF_DATA:
            value = s->sniff_data;
            break;
        case DMA_FIFO_LEVELS:
            value = 0;
            break;
        case DMA_CHAN_ABORT:
            value = 0;
            break;
        default:
            value = 0;
            break;
        }
    }

    qemu_log_mask(LOG_UNIMP, "rp2040.dma: read  "
                  "(size %d, addr 0x%08" HWADDR_PRIx
                  ", offset 0x%04" HWADDR_PRIx ") -> 0x%0*" PRIx32 "\n",
                  size, RP2040_DMA_BASE + addr, offset, size << 1, value);
    return value;
}

static void rp2040_dma_write(void *opaque, hwaddr addr, uint64_t value64,
                             unsigned size)
{
    RP2040DmaState *s = opaque;
    hwaddr alias = addr & ATOMIC_ALIAS_MASK;
    hwaddr offset = addr & 0xfff;
    uint32_t value = value64;
    uint32_t old;
    int i;

    if (offset < RP2040_DMA_NUM_CHANNELS * DMA_CH_SIZE) {
        rp2040_dma_write_channel(s, offset / DMA_CH_SIZE,
                                 offset % DMA_CH_SIZE, value);
    } else {
        switch (offset) {
        case DMA_INTR:
            s->intr &= ~(value & DMA_CHANNEL_MASK);
            rp2040_dma_update_irq(s);
            break;
        case DMA_INTE0:
            s->inte[0] = rp2040_dma_apply_alias(s->inte[0], value, alias) &
                         DMA_CHANNEL_MASK;
            rp2040_dma_update_irq(s);
            break;
        case DMA_INTF0:
            s->intf[0] = rp2040_dma_apply_alias(s->intf[0], value, alias) &
                         DMA_CHANNEL_MASK;
            rp2040_dma_update_irq(s);
            break;
        case DMA_INTE1:
            s->inte[1] = rp2040_dma_apply_alias(s->inte[1], value, alias) &
                         DMA_CHANNEL_MASK;
            rp2040_dma_update_irq(s);
            break;
        case DMA_INTF1:
            s->intf[1] = rp2040_dma_apply_alias(s->intf[1], value, alias) &
                         DMA_CHANNEL_MASK;
            rp2040_dma_update_irq(s);
            break;
        case DMA_TIMER0 ... DMA_TIMER0 + 3 * sizeof(uint32_t):
            i = (offset - DMA_TIMER0) / sizeof(uint32_t);
            s->timer[i] = rp2040_dma_apply_alias(s->timer[i], value, alias);
            break;
        case DMA_MULTI_CHAN_TRIGGER:
            value &= DMA_CHANNEL_MASK;
            for (i = 0; i < RP2040_DMA_NUM_CHANNELS; i++) {
                if (value & BIT(i)) {
                    rp2040_dma_start_channel(s, i, 0);
                }
            }
            break;
        case DMA_SNIFF_CTRL:
            old = s->sniff_ctrl;
            s->sniff_ctrl = rp2040_dma_apply_alias(old, value, alias);
            rp2040_log_nyi("dma", "sniff control",
                           "register is stored but checksum is not computed");
            break;
        case DMA_SNIFF_DATA:
            s->sniff_data = rp2040_dma_apply_alias(s->sniff_data, value,
                                                   alias);
            break;
        case DMA_CHAN_ABORT:
            value &= DMA_CHANNEL_MASK;
            for (i = 0; i < RP2040_DMA_NUM_CHANNELS; i++) {
                if (value & BIT(i)) {
                    s->chan[i].ctrl &= ~DMA_CTRL_BUSY;
                    s->chan[i].trans_count = 0;
                }
            }
            break;
        default:
            break;
        }
    }

    qemu_log_mask(LOG_UNIMP, "rp2040.dma: write "
                  "(size %d, addr 0x%08" HWADDR_PRIx
                  ", offset 0x%04" HWADDR_PRIx
                  ", value 0x%0*" PRIx64 ")\n",
                  size, RP2040_DMA_BASE + addr, offset, size << 1, value64);
}

static const MemoryRegionOps rp2040_dma_ops = {
    .read = rp2040_dma_read,
    .write = rp2040_dma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void rp2040_dma_reset(DeviceState *dev)
{
    RP2040DmaState *s = RP2040_DMA(dev);
    int i;

    for (i = 0; i < RP2040_DMA_NUM_CHANNELS; i++) {
        s->chan[i].read_addr = 0;
        s->chan[i].write_addr = 0;
        s->chan[i].trans_count = 0;
        s->chan[i].reload_count = 0;
        s->chan[i].ctrl = i << DMA_CTRL_CHAIN_TO_SHIFT;
    }
    s->intr = 0;
    s->inte[0] = 0;
    s->inte[1] = 0;
    s->intf[0] = 0;
    s->intf[1] = 0;
    memset(s->timer, 0, sizeof(s->timer));
    s->sniff_ctrl = 0;
    s->sniff_data = 0;
    rp2040_dma_update_irq(s);
}

static void rp2040_dma_init(Object *obj)
{
    RP2040DmaState *s = RP2040_DMA(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    int i;

    memory_region_init_io(&s->iomem, obj, &rp2040_dma_ops, s,
                          TYPE_RP2040_DMA, RP2040_DMA_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    for (i = 0; i < RP2040_DMA_NUM_IRQS; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }
}

static void rp2040_dma_realize(DeviceState *dev, Error **errp)
{
    RP2040DmaState *s = RP2040_DMA(dev);

    if (!s->dma_mr) {
        error_setg(errp, "memory property was not set");
        return;
    }

    address_space_init(&s->dma_as, s->dma_mr, "rp2040-dma-memory");
}

static const VMStateDescription rp2040_dma_vmstate = {
    .name = TYPE_RP2040_DMA,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    }
};

static const Property rp2040_dma_properties[] = {
    DEFINE_PROP_LINK("memory", RP2040DmaState, dma_mr, TYPE_MEMORY_REGION,
                     MemoryRegion *),
};

static void rp2040_dma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rp2040_dma_realize;
    device_class_set_legacy_reset(dc, rp2040_dma_reset);
    dc->vmsd = &rp2040_dma_vmstate;
    device_class_set_props(dc, rp2040_dma_properties);
}

static const TypeInfo rp2040_dma_info = {
    .name          = TYPE_RP2040_DMA,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RP2040DmaState),
    .instance_init = rp2040_dma_init,
    .class_init    = rp2040_dma_class_init,
};

static void rp2040_dma_register_types(void)
{
    type_register_static(&rp2040_dma_info);
}
type_init(rp2040_dma_register_types)
