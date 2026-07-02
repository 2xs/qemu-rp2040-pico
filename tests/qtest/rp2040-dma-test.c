/*
 * QTest testcase for the RP2040 DMA block.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bitops.h"

#define DMA_BASE                0x50000000
#define DMA_CH_READ_ADDR        0x00
#define DMA_CH_WRITE_ADDR       0x04
#define DMA_CH_TRANS_COUNT      0x08
#define DMA_CH_CTRL_TRIG        0x0c

#define DMA_CTRL_BUSY           BIT(24)
#define DMA_CTRL_TREQ_SEL_SHIFT 15
#define DMA_CTRL_CHAIN_TO_SHIFT 11
#define DMA_CTRL_INCR_WRITE     BIT(5)
#define DMA_CTRL_DATA_SIZE_8    (0 << 2)
#define DMA_CTRL_EN             BIT(0)

#define DREQ_XIP_SSIRX          39

#define XIP_SSI_BASE            0x18000000
#define XIP_SSI_DR0             0x60

#define SRAM_BASE               0x20000000

static QTestState *rp2040_start(void)
{
    return qtest_init("-machine raspi-pico");
}

static void test_dma_xip_ssi_rx_dreq(void)
{
    QTestState *qts = rp2040_start();
    uint32_t ctrl = DMA_CTRL_EN | DMA_CTRL_INCR_WRITE | DMA_CTRL_DATA_SIZE_8 |
                    (DREQ_XIP_SSIRX << DMA_CTRL_TREQ_SEL_SHIFT);

    qtest_writel(qts, SRAM_BASE, 0xffffffff);
    qtest_writel(qts, DMA_BASE + DMA_CH_READ_ADDR, XIP_SSI_BASE + XIP_SSI_DR0);
    qtest_writel(qts, DMA_BASE + DMA_CH_WRITE_ADDR, SRAM_BASE);
    qtest_writel(qts, DMA_BASE + DMA_CH_TRANS_COUNT, 4);
    qtest_writel(qts, DMA_BASE + DMA_CH_CTRL_TRIG, ctrl);

    g_assert_cmphex(qtest_readl(qts, DMA_BASE + DMA_CH_CTRL_TRIG) &
                    DMA_CTRL_BUSY, ==, DMA_CTRL_BUSY);
    g_assert_cmphex(qtest_readl(qts, DMA_BASE + DMA_CH_TRANS_COUNT), ==, 4);

    qtest_writel(qts, XIP_SSI_BASE + XIP_SSI_DR0, 0x03);
    qtest_writel(qts, XIP_SSI_BASE + XIP_SSI_DR0, 0x00);
    qtest_writel(qts, XIP_SSI_BASE + XIP_SSI_DR0, 0x00);
    qtest_writel(qts, XIP_SSI_BASE + XIP_SSI_DR0, 0x00);

    g_assert_cmphex(qtest_readl(qts, DMA_BASE + DMA_CH_TRANS_COUNT), ==, 0);
    g_assert_cmphex(qtest_readl(qts, DMA_BASE + DMA_CH_CTRL_TRIG) &
                    DMA_CTRL_BUSY, ==, 0);
    g_assert_cmphex(qtest_readl(qts, SRAM_BASE), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/rp2040-dma/xip-ssi-rx-dreq",
                   test_dma_xip_ssi_rx_dreq);

    return g_test_run();
}
