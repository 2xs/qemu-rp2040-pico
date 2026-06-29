/*
 * RP2040 XIP/SSI flash controller emulation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SSI_RP2040_XIP_H
#define HW_SSI_RP2040_XIP_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RP2040_XIP "rp2040-xip"
OBJECT_DECLARE_SIMPLE_TYPE(RP2040XipState, RP2040_XIP)

#define RP2040_XIP_CTRL_BASE 0x14000000
#define RP2040_XIP_SSI_BASE  0x18000000
#define RP2040_XIP_CTRL_SIZE 0x4000
#define RP2040_XIP_SSI_SIZE  0x4000

struct RP2040XipState {
    SysBusDevice parent_obj;

    MemoryRegion xip;
    MemoryRegion ctrl;
    MemoryRegion ssi;

    uint32_t flash_size;
    char *flash_file;
    uint8_t *storage;
    bool xip_writable;

    uint32_t xip_ctrl;

    uint32_t ctrlr0;
    uint32_t ctrlr1;
    uint32_t ssienr;
    uint32_t ser;
    uint32_t baudr;
    uint32_t txftlr;
    uint32_t rxftlr;
    uint32_t imr;
    uint32_t spi_ctrlr0;

    bool write_enable;
    bool busy;
    uint8_t tx[260];
    unsigned tx_len;
    uint8_t rx[16];
    unsigned rx_len;
    unsigned rx_pos;
};

void rp2040_xip_set_writable(RP2040XipState *s, bool writable);
void rp2040_xip_load_image(RP2040XipState *s, const char *filename,
                           Error **errp);

#endif
