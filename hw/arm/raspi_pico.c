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

#define PICO_FLASH_SIZE (2 * MiB)

struct RaspiPicoMachineState {
    MachineState parent_obj;

    RP2040State soc;
    MemoryRegion flash;
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

static void raspi_pico_init_flash(RaspiPicoMachineState *s,
                                  MemoryRegion *system_memory,
                                  Error **errp)
{
    g_autofree gchar *contents = NULL;
    gsize contents_len = 0;
    uint8_t *flash;

    if (!memory_region_init_ram(&s->flash, NULL, "raspi-pico.flash",
                                PICO_FLASH_SIZE, errp)) {
        return;
    }

    flash = memory_region_get_ram_ptr(&s->flash);
    memset(flash, 0xff, PICO_FLASH_SIZE);

    if (s->flash_file) {
        if (!g_file_get_contents(s->flash_file, &contents, &contents_len,
                                 NULL)) {
            error_setg(errp, "could not load flash file '%s'",
                       s->flash_file);
            return;
        }

        if (contents_len > PICO_FLASH_SIZE) {
            error_setg(errp, "flash file '%s' is %" G_GSIZE_FORMAT
                       " bytes, larger than %" G_GSIZE_FORMAT
                       " byte Pico flash",
                       s->flash_file, contents_len, (gsize)PICO_FLASH_SIZE);
            return;
        }

        memcpy(flash, contents, contents_len);
    }

    memory_region_add_subregion(system_memory, RP2040_XIP_BASE, &s->flash);
}

static void raspi_pico_init(MachineState *machine)
{
    RaspiPicoMachineState *s = RASPI_PICO_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();

    object_initialize_child(OBJECT(machine), "soc", &s->soc, TYPE_RP2040);
    qdev_prop_set_chr(DEVICE(&s->soc), "serial0", serial_hd(0));
    object_property_set_link(OBJECT(&s->soc), "memory",
                             OBJECT(system_memory), &error_fatal);

    raspi_pico_init_flash(s, system_memory, &error_fatal);

    sysbus_realize(SYS_BUS_DEVICE(&s->soc), &error_fatal);

    /*
     * For now, -kernel images are loaded directly into the XIP window.
     * A faithful boot ROM and SSI/QSPI model will be added later.
     */
    armv7m_load_kernel(s->soc.armv7m.cpu, machine->kernel_filename,
                       RP2040_XIP_BASE, PICO_FLASH_SIZE);
    memory_region_set_readonly(&s->flash, true);
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
