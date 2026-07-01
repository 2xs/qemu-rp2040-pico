/*
 * RP2040 DMA emulation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DMA_RP2040_DMA_H
#define HW_DMA_RP2040_DMA_H

#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_RP2040_DMA "rp2040-dma"
OBJECT_DECLARE_SIMPLE_TYPE(RP2040DmaState, RP2040_DMA)

#define RP2040_DMA_BASE 0x50000000
#define RP2040_DMA_SIZE 0x4000
#define RP2040_DMA_NUM_CHANNELS 12
#define RP2040_DMA_NUM_IRQS 2

typedef struct RP2040DmaChannel {
    uint32_t read_addr;
    uint32_t write_addr;
    uint32_t trans_count;
    uint32_t reload_count;
    uint32_t ctrl;
} RP2040DmaChannel;

struct RP2040DmaState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegion *dma_mr;
    AddressSpace dma_as;
    qemu_irq irq[RP2040_DMA_NUM_IRQS];

    RP2040DmaChannel chan[RP2040_DMA_NUM_CHANNELS];
    uint32_t intr;
    uint32_t inte[RP2040_DMA_NUM_IRQS];
    uint32_t intf[RP2040_DMA_NUM_IRQS];
    uint32_t timer[4];
    uint32_t sniff_ctrl;
    uint32_t sniff_data;
};

#endif
