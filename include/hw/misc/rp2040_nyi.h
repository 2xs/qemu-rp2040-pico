/*
 * RP2040 "not yet implemented" diagnostics
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_RP2040_NYI_H
#define HW_MISC_RP2040_NYI_H

void rp2040_log_nyi(const char *component, const char *feature,
                    const char *detail);

#endif
