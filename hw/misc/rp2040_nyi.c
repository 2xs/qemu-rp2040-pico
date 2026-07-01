/*
 * RP2040 "not yet implemented" diagnostics
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/rp2040_nyi.h"
#include "qemu/log.h"

void rp2040_log_nyi(const char *component, const char *feature,
                    const char *detail)
{
    qemu_log_mask(LOG_UNIMP, "Not yet implemented: rp2040.%s: %s%s%s\n",
                  component, feature, detail ? ": " : "",
                  detail ? detail : "");
}
