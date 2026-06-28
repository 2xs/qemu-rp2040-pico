/*
 * Raspberry Pi Pico machine
 *
 * Copyright (c) 2021 Linaro Ltd
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/arm/rp2040.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "qom/object.h"

#define TYPE_RASPI_PICO_MACHINE MACHINE_TYPE_NAME("raspi-pico")
OBJECT_DECLARE_SIMPLE_TYPE(RaspiPicoMachineState, RASPI_PICO_MACHINE)

struct RaspiPicoMachineState {
    MachineState parent_obj;

    RP2040State soc;
    char *flash_file;
};

static char *raspi_pico_get_flash_file(Object *obj, Error **errp)
{
    RaspiPicoMachineState *s = RASPI_PICO_MACHINE(obj);

    return g_strdup(s->flash_file ?: "");
}

static void raspi_pico_set_flash_file(Object *obj, const char *value,
                                      Error **errp)
{
    RaspiPicoMachineState *s = RASPI_PICO_MACHINE(obj);

    g_free(s->flash_file);
    s->flash_file = g_strdup(value);
}

static void raspi_pico_init(MachineState *machine)
{
    RaspiPicoMachineState *s = RASPI_PICO_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();

    object_initialize_child(OBJECT(machine), "soc", &s->soc, TYPE_RP2040);
    qdev_prop_set_chr(DEVICE(&s->soc), "serial0", serial_hd(0));
    if (s->flash_file) {
        qdev_prop_set_string(DEVICE(&s->soc.xip), "flash-file",
                             s->flash_file);
    }
    object_property_set_link(OBJECT(&s->soc), "memory",
                             OBJECT(system_memory), &error_fatal);

    sysbus_realize(SYS_BUS_DEVICE(&s->soc), &error_fatal);

    /*
     * For now, -kernel images are loaded directly into the XIP window.
     * A faithful boot ROM and SSI/QSPI model will be added later.
     */
    rp2040_xip_load_image(&s->soc.xip, machine->kernel_filename,
                          &error_fatal);
    armv7m_load_kernel(s->soc.armv7m.cpu, NULL, RP2040_XIP_BASE, 2 * MiB);
    rp2040_xip_set_writable(&s->soc.xip, false);
}

static void raspi_pico_machine_finalize(Object *obj)
{
    RaspiPicoMachineState *s = RASPI_PICO_MACHINE(obj);

    g_free(s->flash_file);
}

static void raspi_pico_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Raspberry Pi Pico (Cortex-M0+)";
    mc->init = raspi_pico_init;
    mc->max_cpus = 1;
    mc->no_parallel = 1;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;

    object_class_property_add_str(oc, "flash-file",
                                  raspi_pico_get_flash_file,
                                  raspi_pico_set_flash_file);
    object_class_property_set_description(oc, "flash-file",
                                          "Load initial XIP flash contents "
                                          "from a raw host file");
}

static const TypeInfo raspi_pico_machine_info = {
    .name = TYPE_RASPI_PICO_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(RaspiPicoMachineState),
    .class_init = raspi_pico_machine_class_init,
    .instance_finalize = raspi_pico_machine_finalize,
    .interfaces = arm_machine_interfaces,
};

static void raspi_pico_machine_init(void)
{
    type_register_static(&raspi_pico_machine_info);
}
type_init(raspi_pico_machine_init)
