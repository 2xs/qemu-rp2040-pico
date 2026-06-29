/*
 * RP2040 SoC emulation
 *
 * Copyright (c) 2021 Linaro Ltd
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/arm/rp2040.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/loader.h"
#include "hw/misc/unimp.h"
#include "qemu/datadir.h"
#include "qemu/log.h"
#include "target/arm/cpu-qom.h"

#define RP2040_UART0_BASE 0x40034000
#define RP2040_UART0_IRQ  20

#define USBCTRL_ADDR_ENDP       0x00
#define USBCTRL_SIE_CTRL        0x4c
#define USBCTRL_SIE_STATUS      0x50
#define USBCTRL_INT_EP_CTRL     0x54
#define USBCTRL_BUFF_STATUS     0x58
#define USBCTRL_BUFF_CPU_HANDLE 0x5c
#define USBCTRL_EP_ABORT        0x60
#define USBCTRL_EP_ABORT_DONE   0x64
#define USBCTRL_EP_STALL_ARM    0x68
#define USBCTRL_NAK_POLL        0x6c
#define USBCTRL_EP_STATUS       0x70
#define USBCTRL_USB_MUXING      0x74
#define USBCTRL_USB_PWR         0x78
#define USBCTRL_USBPHY_DIRECT   0x7c
#define USBCTRL_USBPHY_TRIM     0x80
#define USBCTRL_INTR            0x8c
#define USBCTRL_INTE            0x90
#define USBCTRL_INTF            0x94
#define USBCTRL_INTS            0x98

#define USBCTRL_SIE_STATUS_VBUS_DETECTED BIT(11)

#define ATOMIC_ALIAS_MASK 0x3000
#define ATOMIC_XOR        0x1000
#define ATOMIC_SET        0x2000
#define ATOMIC_CLR        0x3000

/*
 * Temporary boot ROM used until a faithful RP2040 boot ROM is modeled.
 * It uses a fixed stack top, loads the reset handler from the XIP vector
 * table, and branches to it.
 */
static const uint8_t rp2040_bootrom[] = {
    0x00, 0x20, 0x04, 0x20, /* initial SP: 0x20042000 */
    0x09, 0x00, 0x00, 0x00, /* reset handler: 0x00000009 */
    0x03, 0x48,             /* ldr r0, [pc, #12] */
    0x04, 0x49,             /* ldr r1, [pc, #16] */
    0x01, 0x60,             /* str r1, [r0] */
    0x04, 0x48,             /* ldr r0, [pc, #16] */
    0x01, 0x68,             /* ldr r1, [r0] */
    0x08, 0x47,             /* bx r1 */
    0xfe, 0xe7,             /* b . */
    0x00, 0x00,             /* padding for word-aligned literals */
    0x08, 0xed, 0x00, 0xe0, /* VTOR address: 0xe000ed08 */
    0x00, 0x00, 0x00, 0x10, /* XIP vector table: 0x10000000 */
    0x04, 0x00, 0x00, 0x10, /* XIP reset vector address: 0x10000004 */
};

static const struct {
    const char *name;
    hwaddr base;
    hwaddr size;
} rp2040_unimplemented[] = {
    { "rp2040.psm",      0x40010000, 0x4000 },
    { "rp2040.iobank0",  0x40014000, 0x4000 },
    { "rp2040.ioqspi",   0x40018000, 0x4000 },
    { "rp2040.padsbank0", 0x4001c000, 0x4000 },
    { "rp2040.padsqspi", 0x40020000, 0x4000 },
    { "rp2040.busctrl",  0x40030000, 0x4000 },
    { "rp2040.uart1",    0x40038000, 0x4000 },
    { "rp2040.spi0",     0x4003c000, 0x4000 },
    { "rp2040.spi1",     0x40040000, 0x4000 },
    { "rp2040.i2c0",     0x40044000, 0x4000 },
    { "rp2040.i2c1",     0x40048000, 0x4000 },
    { "rp2040.adc",      0x4004c000, 0x4000 },
    { "rp2040.pwm",      0x40050000, 0x4000 },
    { "rp2040.timer",    0x40054000, 0x4000 },
    { "rp2040.rtc",      0x4005c000, 0x4000 },
    { "rp2040.dma",      0x50000000, 0x1000 },
    { "rp2040.pio0",     0x50200000, 0x10000 },
    { "rp2040.pio1",     0x50300000, 0x10000 },
    { "rp2040.sio",      0xd0000000, 0x1000 },
};

static uint32_t rp2040_apply_atomic_alias(uint32_t old, uint32_t value,
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

static MemTxResult rp2040_powered_off_read(void *opaque, hwaddr addr,
                                           uint64_t *data, unsigned size,
                                           MemTxAttrs attrs)
{
    *data = 0;
    return MEMTX_ERROR;
}

static MemTxResult rp2040_powered_off_write(void *opaque, hwaddr addr,
                                            uint64_t data, unsigned size,
                                            MemTxAttrs attrs)
{
    return MEMTX_ERROR;
}

static const MemoryRegionOps rp2040_powered_off_ops = {
    .read_with_attrs = rp2040_powered_off_read,
    .write_with_attrs = rp2040_powered_off_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void rp2040_update_mempowerdown(RP2040State *s)
{
    uint32_t mempowerdown;
    int i;

    if (!s->mempowerdown_ready) {
        return;
    }

    mempowerdown = rp2040_syscfg_get_mempowerdown(&s->syscfg);

    for (i = 0; i < ARRAY_SIZE(s->sram_poweroff); i++) {
        memory_region_set_enabled(&s->sram_poweroff[i],
                                  mempowerdown & BIT(i));
    }

    memory_region_set_enabled(&s->usbctrl_dpram_poweroff,
                              mempowerdown & BIT(6));
    memory_region_set_enabled(&s->rom_poweroff, mempowerdown & BIT(7));
}

static void rp2040_update_nmi(RP2040State *s)
{
    uint32_t nmi_mask = rp2040_syscfg_get_proc0_nmi_mask(&s->syscfg);
    bool nmi_level = false;
    int i;

    for (i = 0; i < RP2040_NUM_IRQS; i++) {
        bool irq_level = s->irq_level[i];
        bool route_to_nmi = nmi_mask & BIT(i);

        qemu_set_irq(s->cpu_irq[i], irq_level && !route_to_nmi);
        nmi_level |= irq_level && route_to_nmi;
    }

    qemu_set_irq(s->nmi_irq, nmi_level);
}

static void rp2040_syscfg_update(void *opaque)
{
    RP2040State *s = opaque;

    rp2040_update_mempowerdown(s);
    rp2040_update_nmi(s);
}

static void rp2040_set_irq(void *opaque, int irq, int level)
{
    RP2040State *s = opaque;

    assert(irq >= 0 && irq < RP2040_NUM_IRQS);
    s->irq_level[irq] = level;
    rp2040_update_nmi(s);
}

static uint64_t rp2040_usbctrl_regs_read(void *opaque, hwaddr addr,
                                         unsigned size)
{
    RP2040State *s = opaque;
    hwaddr offset = addr & 0xfff;
    uint64_t value;

    switch (offset) {
    case USBCTRL_SIE_STATUS:
        value = s->usbctrl_reg[offset / sizeof(uint32_t)] |
                USBCTRL_SIE_STATUS_VBUS_DETECTED;
        break;
    case USBCTRL_BUFF_CPU_HANDLE:
    case USBCTRL_EP_ABORT_DONE:
    case USBCTRL_INTR:
        value = s->usbctrl_reg[offset / sizeof(uint32_t)];
        break;
    case USBCTRL_INTS:
        value = (s->usbctrl_reg[USBCTRL_INTR / sizeof(uint32_t)] |
                 s->usbctrl_reg[USBCTRL_INTF / sizeof(uint32_t)]) &
                s->usbctrl_reg[USBCTRL_INTE / sizeof(uint32_t)];
        break;
    default:
        if (offset < sizeof(s->usbctrl_reg)) {
            value = s->usbctrl_reg[offset / sizeof(uint32_t)];
        } else {
            value = 0;
        }
        break;
    }

    qemu_log_mask(LOG_UNIMP, "rp2040.usbctrl_regs: read  "
                  "(size %d, addr 0x%08" HWADDR_PRIx
                  ", offset 0x%04" HWADDR_PRIx ") -> 0x%0*" PRIx64 "\n",
                  size, RP2040_USBCTRL_REGS_BASE + addr, offset,
                  size << 1, value);
    return value;
}

static void rp2040_usbctrl_regs_write(void *opaque, hwaddr addr,
                                      uint64_t value64, unsigned size)
{
    RP2040State *s = opaque;
    hwaddr alias = addr & ATOMIC_ALIAS_MASK;
    hwaddr offset = addr & 0xfff;
    uint32_t value = value64;
    uint32_t old;

    if (offset < sizeof(s->usbctrl_reg)) {
        old = s->usbctrl_reg[offset / sizeof(uint32_t)];
        s->usbctrl_reg[offset / sizeof(uint32_t)] =
            rp2040_apply_atomic_alias(old, value, alias);
    }

    qemu_log_mask(LOG_UNIMP, "rp2040.usbctrl_regs: write "
                  "(size %d, addr 0x%08" HWADDR_PRIx
                  ", offset 0x%04" HWADDR_PRIx
                  ", value 0x%0*" PRIx64 ")\n",
                  size, RP2040_USBCTRL_REGS_BASE + addr, offset,
                  size << 1, value64);
}

static const MemoryRegionOps rp2040_usbctrl_regs_ops = {
    .read = rp2040_usbctrl_regs_read,
    .write = rp2040_usbctrl_regs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void rp2040_soc_init(Object *obj)
{
    RP2040State *s = RP2040(obj);

    object_initialize_child(obj, "armv7m", &s->armv7m, TYPE_ARMV7M);
    qdev_prop_set_string(DEVICE(&s->armv7m), "cpu-type",
                         ARM_CPU_TYPE_NAME("cortex-m0"));
    qdev_prop_set_uint32(DEVICE(&s->armv7m), "num-irq", 32);

    object_initialize_child(obj, "uart0", &s->uart0, TYPE_PL011);
    object_property_add_alias(obj, "serial0", OBJECT(&s->uart0), "chardev");

    object_initialize_child(obj, "xip", &s->xip, TYPE_RP2040_XIP);
    object_initialize_child(obj, "clocks", &s->clocks, TYPE_RP2040_CLOCKS);
    object_initialize_child(obj, "pll-sys", &s->pll_sys, TYPE_RP2040_PLL);
    qdev_prop_set_string(DEVICE(&s->pll_sys), "trace-name",
                         "rp2040.pll_sys");
    qdev_prop_set_uint32(DEVICE(&s->pll_sys), "base", RP2040_PLL_SYS_BASE);
    qdev_prop_set_uint32(DEVICE(&s->pll_sys), "fallback-hz", 125000000);

    object_initialize_child(obj, "pll-usb", &s->pll_usb, TYPE_RP2040_PLL);
    qdev_prop_set_string(DEVICE(&s->pll_usb), "trace-name",
                         "rp2040.pll_usb");
    qdev_prop_set_uint32(DEVICE(&s->pll_usb), "base", RP2040_PLL_USB_BASE);
    qdev_prop_set_uint32(DEVICE(&s->pll_usb), "fallback-hz", 48000000);

    object_initialize_child(obj, "resets", &s->resets, TYPE_RP2040_RESETS);
    object_initialize_child(obj, "rosc", &s->rosc, TYPE_RP2040_ROSC);
    object_initialize_child(obj, "syscfg", &s->syscfg, TYPE_RP2040_SYSCFG);
    object_initialize_child(obj, "sysinfo", &s->sysinfo, TYPE_RP2040_SYSINFO);
    object_initialize_child(obj, "tbman", &s->tbman, TYPE_RP2040_TBMAN);
    object_initialize_child(obj, "vreg", &s->vreg, TYPE_RP2040_VREG);
    object_initialize_child(obj, "watchdog", &s->watchdog,
                            TYPE_RP2040_WATCHDOG);
    object_initialize_child(obj, "xosc", &s->xosc, TYPE_RP2040_XOSC);

    s->irq = qemu_allocate_irqs(rp2040_set_irq, s, RP2040_NUM_IRQS);
    s->sysclk = clock_new(obj, "sysclk");
}

static void rp2040_soc_realize(DeviceState *dev, Error **errp)
{
    RP2040State *s = RP2040(dev);
    Error *err = NULL;
    g_autofree char *filename = NULL;
    ssize_t image_size;
    int i;

    if (!s->board_memory) {
        error_setg(errp, "memory property was not set");
        return;
    }

    if (!memory_region_init_rom(&s->rom, OBJECT(dev), "rp2040.rom",
                                RP2040_ROM_SIZE, errp)) {
        return;
    }
    memory_region_add_subregion(s->board_memory, RP2040_ROM_BASE, &s->rom);
    memory_region_init_io(&s->rom_poweroff, OBJECT(dev),
                          &rp2040_powered_off_ops, s,
                          "rp2040.rom.poweroff", RP2040_ROM_SIZE);
    memory_region_add_subregion_overlap(s->board_memory, RP2040_ROM_BASE,
                                        &s->rom_poweroff, 1);
    memory_region_set_enabled(&s->rom_poweroff, false);

    if (s->bootrom_file) {
        filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, s->bootrom_file);
        if (!filename) {
            error_setg(errp, "could not find RP2040 boot ROM image '%s'",
                       s->bootrom_file);
            return;
        }

        image_size = load_image_targphys(filename, RP2040_ROM_BASE,
                                         RP2040_ROM_SIZE, errp);
        if (image_size < 0) {
            return;
        }
    } else {
        rom_add_blob_fixed("rp2040.bootrom", rp2040_bootrom,
                           sizeof(rp2040_bootrom), RP2040_ROM_BASE);
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->xip), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->xip), 0, RP2040_XIP_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->xip), 1, RP2040_XIP_CTRL_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->xip), 2, RP2040_XIP_SSI_BASE);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->clocks), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->clocks), 0, RP2040_CLOCKS_BASE);
    clock_set_source(s->sysclk, qdev_get_clock_out(DEVICE(&s->clocks),
                                                   "clk-sys"));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->pll_sys), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->pll_sys), 0, RP2040_PLL_SYS_BASE);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->pll_usb), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->pll_usb), 0, RP2040_PLL_USB_BASE);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->resets), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->resets), 0, RP2040_RESETS_BASE);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->rosc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->rosc), 0, RP2040_ROSC_BASE);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->syscfg), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->syscfg), 0, RP2040_SYSCFG_BASE);
    rp2040_syscfg_set_update_callback(&s->syscfg, rp2040_syscfg_update, s);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->sysinfo), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->sysinfo), 0, RP2040_SYSINFO_BASE);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->tbman), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->tbman), 0, RP2040_TBMAN_BASE);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->vreg), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->vreg), 0, RP2040_VREG_BASE);

    qdev_connect_clock_in(DEVICE(&s->watchdog), "clk-ref",
                          qdev_get_clock_out(DEVICE(&s->clocks), "clk-ref"));
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->watchdog), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->watchdog), 0, RP2040_WATCHDOG_BASE);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->xosc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->xosc), 0, RP2040_XOSC_BASE);

    for (i = 0; i < 4; i++) {
        g_autofree char *name = g_strdup_printf("rp2040.sram%d", i);
        g_autofree char *poweroff_name =
            g_strdup_printf("rp2040.sram%d.poweroff", i);

        if (!memory_region_init_ram(&s->sram[i], OBJECT(dev), name,
                                    RP2040_SRAM_BANK_SIZE, errp)) {
            return;
        }
        memory_region_add_subregion(s->board_memory,
                                    RP2040_SRAM_BASE +
                                    i * RP2040_SRAM_BANK_SIZE,
                                    &s->sram[i]);
        memory_region_init_io(&s->sram_poweroff[i], OBJECT(dev),
                              &rp2040_powered_off_ops, s, poweroff_name,
                              RP2040_SRAM_BANK_SIZE);
        memory_region_add_subregion_overlap(s->board_memory,
                                            RP2040_SRAM_BASE +
                                            i * RP2040_SRAM_BANK_SIZE,
                                            &s->sram_poweroff[i], 1);
        memory_region_set_enabled(&s->sram_poweroff[i], false);
    }

    if (!memory_region_init_ram(&s->sram[4], OBJECT(dev), "rp2040.sram4",
                                RP2040_SRAM_HI_SIZE, errp)) {
        return;
    }
    memory_region_add_subregion(s->board_memory, RP2040_SRAM4_BASE,
                                &s->sram[4]);
    memory_region_init_io(&s->sram_poweroff[4], OBJECT(dev),
                          &rp2040_powered_off_ops, s,
                          "rp2040.sram4.poweroff", RP2040_SRAM_HI_SIZE);
    memory_region_add_subregion_overlap(s->board_memory, RP2040_SRAM4_BASE,
                                        &s->sram_poweroff[4], 1);
    memory_region_set_enabled(&s->sram_poweroff[4], false);

    if (!memory_region_init_ram(&s->sram[5], OBJECT(dev), "rp2040.sram5",
                                RP2040_SRAM_HI_SIZE, errp)) {
        return;
    }
    memory_region_add_subregion(s->board_memory, RP2040_SRAM5_BASE,
                                &s->sram[5]);
    memory_region_init_io(&s->sram_poweroff[5], OBJECT(dev),
                          &rp2040_powered_off_ops, s,
                          "rp2040.sram5.poweroff", RP2040_SRAM_HI_SIZE);
    memory_region_add_subregion_overlap(s->board_memory, RP2040_SRAM5_BASE,
                                        &s->sram_poweroff[5], 1);
    memory_region_set_enabled(&s->sram_poweroff[5], false);

    if (!memory_region_init_ram(&s->usbctrl_dpram, OBJECT(dev),
                                "rp2040.usbctrl_dpram",
                                RP2040_USBCTRL_DPRAM_SIZE, errp)) {
        return;
    }
    memory_region_add_subregion(s->board_memory, RP2040_USBCTRL_DPRAM_BASE,
                                &s->usbctrl_dpram);
    memory_region_init_io(&s->usbctrl_dpram_poweroff, OBJECT(dev),
                          &rp2040_powered_off_ops, s,
                          "rp2040.usbctrl_dpram.poweroff",
                          RP2040_USBCTRL_DPRAM_SIZE);
    memory_region_add_subregion_overlap(s->board_memory,
                                        RP2040_USBCTRL_DPRAM_BASE,
                                        &s->usbctrl_dpram_poweroff, 1);
    memory_region_set_enabled(&s->usbctrl_dpram_poweroff, false);

    s->mempowerdown_ready = true;
    rp2040_update_mempowerdown(s);

    memory_region_init_io(&s->usbctrl_regs, OBJECT(dev),
                          &rp2040_usbctrl_regs_ops, s,
                          "rp2040.usbctrl_regs",
                          RP2040_USBCTRL_REGS_SIZE);
    memory_region_add_subregion(s->board_memory, RP2040_USBCTRL_REGS_BASE,
                                &s->usbctrl_regs);

    for (i = 0; i < ARRAY_SIZE(rp2040_unimplemented); i++) {
        create_unimplemented_device(rp2040_unimplemented[i].name,
                                    rp2040_unimplemented[i].base,
                                    rp2040_unimplemented[i].size);
    }

    qdev_connect_clock_in(DEVICE(&s->armv7m), "cpuclk", s->sysclk);
    object_property_set_link(OBJECT(&s->armv7m), "memory",
                             OBJECT(s->board_memory), &err);
    if (err != NULL) {
        error_propagate(errp, err);
        return;
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->armv7m), errp)) {
        return;
    }
    for (i = 0; i < RP2040_NUM_IRQS; i++) {
        s->cpu_irq[i] = qdev_get_gpio_in(DEVICE(&s->armv7m), i);
    }
    s->nmi_irq = qdev_get_gpio_in_named(DEVICE(&s->armv7m), "NMI", 0);
    rp2040_update_nmi(s);

    qdev_connect_clock_in(DEVICE(&s->uart0), "clk",
                          qdev_get_clock_out(DEVICE(&s->clocks), "clk-peri"));
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->uart0), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->uart0), 0, RP2040_UART0_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->uart0), 0, s->irq[RP2040_UART0_IRQ]);
}

static const Property rp2040_soc_properties[] = {
    DEFINE_PROP_LINK("memory", RP2040State, board_memory, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_STRING("bootrom-file", RP2040State, bootrom_file),
};

static void rp2040_soc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rp2040_soc_realize;
    device_class_set_props(dc, rp2040_soc_properties);
}

static const TypeInfo rp2040_soc_info = {
    .name          = TYPE_RP2040,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RP2040State),
    .instance_init = rp2040_soc_init,
    .class_init    = rp2040_soc_class_init,
};

static void rp2040_soc_types(void)
{
    type_register_static(&rp2040_soc_info);
}
type_init(rp2040_soc_types)
