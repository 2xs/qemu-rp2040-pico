/*
 * QTest testcase for the RP2040 SIO block.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bitops.h"

#define SIO_BASE                0xd0000000
#define SIO_CPUID               0x000
#define SIO_GPIO_HI_IN          0x008
#define SIO_GPIO_OUT            0x010
#define SIO_GPIO_OUT_SET        0x014
#define SIO_GPIO_OUT_CLR        0x018
#define SIO_GPIO_OUT_XOR        0x01c
#define SIO_GPIO_HI_OUT         0x030
#define SIO_GPIO_HI_OUT_SET     0x034
#define SIO_GPIO_HI_OUT_CLR     0x038
#define SIO_GPIO_HI_OUT_XOR     0x03c
#define SIO_FIFO_ST             0x050
#define SIO_FIFO_RD             0x058
#define SIO_SPINLOCK_ST         0x05c
#define SIO_SPINLOCK0           0x100

static QTestState *rp2040_start(void)
{
    return qtest_init("-machine raspi-pico");
}

static void test_sio_reset_values(void)
{
    QTestState *qts = rp2040_start();

    g_assert_cmphex(qtest_readl(qts, SIO_BASE + SIO_CPUID), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SIO_BASE + SIO_GPIO_HI_IN), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SIO_BASE + SIO_GPIO_OUT), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SIO_BASE + SIO_GPIO_HI_OUT), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SIO_BASE + SIO_FIFO_ST), ==, BIT(1));
    g_assert_cmphex(qtest_readl(qts, SIO_BASE + SIO_SPINLOCK_ST), ==, 0);

    qtest_quit(qts);
}

static void test_sio_gpio_alias_registers(void)
{
    QTestState *qts = rp2040_start();

    qtest_writel(qts, SIO_BASE + SIO_GPIO_OUT, BIT(3));
    qtest_writel(qts, SIO_BASE + SIO_GPIO_OUT_SET, BIT(5));
    qtest_writel(qts, SIO_BASE + SIO_GPIO_OUT_CLR, BIT(3));
    qtest_writel(qts, SIO_BASE + SIO_GPIO_OUT_XOR, BIT(7));
    g_assert_cmphex(qtest_readl(qts, SIO_BASE + SIO_GPIO_OUT), ==,
                    BIT(5) | BIT(7));

    qtest_writel(qts, SIO_BASE + SIO_GPIO_HI_OUT, BIT(0));
    qtest_writel(qts, SIO_BASE + SIO_GPIO_HI_OUT_SET, BIT(2));
    qtest_writel(qts, SIO_BASE + SIO_GPIO_HI_OUT_CLR, BIT(0));
    qtest_writel(qts, SIO_BASE + SIO_GPIO_HI_OUT_XOR, BIT(5));
    g_assert_cmphex(qtest_readl(qts, SIO_BASE + SIO_GPIO_HI_OUT), ==,
                    BIT(2) | BIT(5));

    qtest_quit(qts);
}

static void test_sio_fifo_empty_read_sets_roe(void)
{
    QTestState *qts = rp2040_start();

    qtest_readl(qts, SIO_BASE + SIO_FIFO_RD);
    g_assert_cmphex(qtest_readl(qts, SIO_BASE + SIO_FIFO_ST), ==,
                    BIT(3) | BIT(1));

    qtest_writel(qts, SIO_BASE + SIO_FIFO_ST, BIT(3));
    g_assert_cmphex(qtest_readl(qts, SIO_BASE + SIO_FIFO_ST), ==, BIT(1));

    qtest_quit(qts);
}

static void test_sio_spinlock_claim_release(void)
{
    QTestState *qts = rp2040_start();

    g_assert_cmphex(qtest_readl(qts, SIO_BASE + SIO_SPINLOCK0), ==, BIT(0));
    g_assert_cmphex(qtest_readl(qts, SIO_BASE + SIO_SPINLOCK0), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SIO_BASE + SIO_SPINLOCK_ST), ==, BIT(0));

    qtest_writel(qts, SIO_BASE + SIO_SPINLOCK0, 0);
    g_assert_cmphex(qtest_readl(qts, SIO_BASE + SIO_SPINLOCK_ST), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/rp2040-sio/reset-values", test_sio_reset_values);
    qtest_add_func("/rp2040-sio/gpio-alias-registers",
                   test_sio_gpio_alias_registers);
    qtest_add_func("/rp2040-sio/fifo-empty-read-sets-roe",
                   test_sio_fifo_empty_read_sets_roe);
    qtest_add_func("/rp2040-sio/spinlock-claim-release",
                   test_sio_spinlock_claim_release);

    return g_test_run();
}
