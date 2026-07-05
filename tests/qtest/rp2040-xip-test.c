/*
 * QTest testcase for the RP2040 XIP/SSI block.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define XIP_SSI_BASE 0x18000000
#define SSI_DR0      0x60

static QTestState *rp2040_start(const char *machine_args)
{
    if (machine_args) {
        return qtest_initf("-machine raspi-pico,%s", machine_args);
    }
    return qtest_init("-machine raspi-pico");
}

static void read_flash_uid(QTestState *qts, uint8_t *uid)
{
    int i;

    qtest_writel(qts, XIP_SSI_BASE + SSI_DR0, 0x4b);
    qtest_readl(qts, XIP_SSI_BASE + SSI_DR0);
    for (i = 0; i < 4; i++) {
        qtest_writel(qts, XIP_SSI_BASE + SSI_DR0, 0);
        qtest_readl(qts, XIP_SSI_BASE + SSI_DR0);
    }
    for (i = 0; i < 8; i++) {
        qtest_writel(qts, XIP_SSI_BASE + SSI_DR0, 0);
        uid[i] = qtest_readl(qts, XIP_SSI_BASE + SSI_DR0);
    }
}

static void test_flash_uid_default(void)
{
    static const uint8_t expected[] = {
        0x3e, 0xb8, 0xa7, 0x49, 0x3f, 0xcc, 0x06, 0x08,
    };
    QTestState *qts = rp2040_start(NULL);
    uint8_t uid[8];

    read_flash_uid(qts, uid);
    g_assert_cmpmem(uid, sizeof(uid), expected, sizeof(expected));

    qtest_quit(qts);
}

static void test_flash_uid_machine_option(void)
{
    static const uint8_t expected[] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    };
    QTestState *qts = rp2040_start("flash-uid=0011223344556677");
    uint8_t uid[8];

    read_flash_uid(qts, uid);
    g_assert_cmpmem(uid, sizeof(uid), expected, sizeof(expected));

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/rp2040-xip/flash-uid-default",
                   test_flash_uid_default);
    qtest_add_func("/rp2040-xip/flash-uid-machine-option",
                   test_flash_uid_machine_option);

    return g_test_run();
}
