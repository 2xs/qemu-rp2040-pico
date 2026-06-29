/*
 * RP2040 single-cycle IO block emulation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_RP2040_SIO_H
#define HW_MISC_RP2040_SIO_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RP2040_SIO "rp2040-sio"
OBJECT_DECLARE_SIMPLE_TYPE(RP2040SioState, RP2040_SIO)

#define RP2040_SIO_BASE 0xd0000000

struct RP2040SioState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t gpio_in;
    uint32_t gpio_hi_in;
    uint32_t gpio_out;
    uint32_t gpio_oe;
    uint32_t gpio_hi_out;
    uint32_t gpio_hi_oe;
    uint32_t fifo_sticky;
    uint32_t spinlock_st;
};

#endif
