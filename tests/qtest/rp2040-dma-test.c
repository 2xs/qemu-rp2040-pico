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
#define DMA_INTR                0x400
#define DMA_INTE0               0x404
#define DMA_INTS0               0x40c
#define DMA_SNIFF_CTRL          0x434
#define DMA_SNIFF_DATA          0x438
#define DMA_CHAN_ABORT          0x444

#define DMA_CTRL_AHB_ERROR      BIT(31)
#define DMA_CTRL_READ_ERROR     BIT(30)
#define DMA_CTRL_WRITE_ERROR    BIT(29)
#define DMA_CTRL_BUSY           BIT(24)
#define DMA_CTRL_SNIFF_EN       BIT(23)
#define DMA_CTRL_TREQ_SEL_SHIFT 15
#define DMA_CTRL_CHAIN_TO_SHIFT 11
#define DMA_CTRL_RING_SEL       BIT(10)
#define DMA_CTRL_RING_SIZE_SHIFT 6
#define DMA_CTRL_INCR_WRITE     BIT(5)
#define DMA_CTRL_INCR_READ      BIT(4)
#define DMA_CTRL_DATA_SIZE_8    (0 << 2)
#define DMA_CTRL_DATA_SIZE_32   (2 << 2)
#define DMA_CTRL_EN             BIT(0)

#define DMA_SNIFF_CTRL_OUT_INV  BIT(11)
#define DMA_SNIFF_CTRL_OUT_REV  BIT(10)
#define DMA_SNIFF_CTRL_CALC_SUM (0xf << 5)
#define DMA_SNIFF_CTRL_EN       BIT(0)

#define DREQ_XIP_SSIRX          39
#define DREQ_FORCE              63

#define XIP_SSI_BASE            0x18000000
#define XIP_SSI_DR0             0x60

#define SRAM_BASE               0x20000000
#define BAD_DMA_ADDR            0x60000000

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

static void test_dma_write_ring_wrap(void)
{
    QTestState *qts = rp2040_start();
    const uint8_t source[] = { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15 };
    const uint8_t initial[] = { 0xaa, 0xaa, 0xaa, 0xaa };
    uint8_t dest[sizeof(initial)];
    uint32_t read_addr = SRAM_BASE + 0x80;
    uint32_t write_addr = SRAM_BASE + 0x100;
    uint32_t ctrl = DMA_CTRL_EN | DMA_CTRL_INCR_READ | DMA_CTRL_INCR_WRITE |
                    DMA_CTRL_RING_SEL | DMA_CTRL_DATA_SIZE_8 |
                    (2 << DMA_CTRL_RING_SIZE_SHIFT) |
                    (DREQ_FORCE << DMA_CTRL_TREQ_SEL_SHIFT);

    qtest_memwrite(qts, read_addr, source, sizeof(source));
    qtest_memwrite(qts, write_addr, initial, sizeof(initial));
    qtest_writel(qts, DMA_BASE + DMA_CH_READ_ADDR, read_addr);
    qtest_writel(qts, DMA_BASE + DMA_CH_WRITE_ADDR, write_addr);
    qtest_writel(qts, DMA_BASE + DMA_CH_TRANS_COUNT, sizeof(source));
    qtest_writel(qts, DMA_BASE + DMA_CH_CTRL_TRIG, ctrl);

    qtest_memread(qts, write_addr, dest, sizeof(dest));
    g_assert_cmphex(dest[0], ==, 0x14);
    g_assert_cmphex(dest[1], ==, 0x15);
    g_assert_cmphex(dest[2], ==, 0x12);
    g_assert_cmphex(dest[3], ==, 0x13);
    g_assert_cmphex(qtest_readl(qts, DMA_BASE + DMA_CH_TRANS_COUNT), ==, 0);
    g_assert_cmphex(qtest_readl(qts, DMA_BASE + DMA_CH_READ_ADDR), ==,
                    read_addr + sizeof(source));
    g_assert_cmphex(qtest_readl(qts, DMA_BASE + DMA_CH_WRITE_ADDR), ==,
                    write_addr + 2);

    qtest_quit(qts);
}

static void test_dma_channel_abort(void)
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

    qtest_writel(qts, DMA_BASE + DMA_CHAN_ABORT, BIT(0));

    g_assert_cmphex(qtest_readl(qts, DMA_BASE + DMA_CHAN_ABORT), ==, 0);
    g_assert_cmphex(qtest_readl(qts, DMA_BASE + DMA_CH_CTRL_TRIG) &
                    DMA_CTRL_BUSY, ==, 0);
    g_assert_cmphex(qtest_readl(qts, DMA_BASE + DMA_CH_TRANS_COUNT), ==, 0);

    qtest_writel(qts, XIP_SSI_BASE + XIP_SSI_DR0, 0x03);
    qtest_writel(qts, XIP_SSI_BASE + XIP_SSI_DR0, 0x00);
    qtest_writel(qts, XIP_SSI_BASE + XIP_SSI_DR0, 0x00);
    qtest_writel(qts, XIP_SSI_BASE + XIP_SSI_DR0, 0x00);

    g_assert_cmphex(qtest_readl(qts, SRAM_BASE), ==, 0xffffffff);

    qtest_quit(qts);
}

static void test_dma_sniff_sum(void)
{
    QTestState *qts = rp2040_start();
    const uint8_t source[] = { 1, 2, 3, 4 };
    uint32_t read_addr = SRAM_BASE + 0x180;
    uint32_t write_addr = SRAM_BASE + 0x190;
    uint32_t ctrl = DMA_CTRL_EN | DMA_CTRL_INCR_READ | DMA_CTRL_INCR_WRITE |
                    DMA_CTRL_SNIFF_EN | DMA_CTRL_DATA_SIZE_8 |
                    (DREQ_FORCE << DMA_CTRL_TREQ_SEL_SHIFT);

    qtest_memwrite(qts, read_addr, source, sizeof(source));
    qtest_writel(qts, DMA_BASE + DMA_SNIFF_DATA, 0x100);
    qtest_writel(qts, DMA_BASE + DMA_SNIFF_CTRL,
                 DMA_SNIFF_CTRL_EN | DMA_SNIFF_CTRL_CALC_SUM);
    qtest_writel(qts, DMA_BASE + DMA_CH_READ_ADDR, read_addr);
    qtest_writel(qts, DMA_BASE + DMA_CH_WRITE_ADDR, write_addr);
    qtest_writel(qts, DMA_BASE + DMA_CH_TRANS_COUNT, sizeof(source));
    qtest_writel(qts, DMA_BASE + DMA_CH_CTRL_TRIG, ctrl);

    g_assert_cmphex(qtest_readl(qts, DMA_BASE + DMA_SNIFF_DATA), ==, 0x10a);

    qtest_writel(qts, DMA_BASE + DMA_SNIFF_CTRL,
                 DMA_SNIFF_CTRL_EN | DMA_SNIFF_CTRL_CALC_SUM |
                 DMA_SNIFF_CTRL_OUT_REV);
    g_assert_cmphex(qtest_readl(qts, DMA_BASE + DMA_SNIFF_DATA), ==,
                    0x50800000);

    qtest_writel(qts, DMA_BASE + DMA_SNIFF_CTRL,
                 DMA_SNIFF_CTRL_EN | DMA_SNIFF_CTRL_CALC_SUM |
                 DMA_SNIFF_CTRL_OUT_INV);
    g_assert_cmphex(qtest_readl(qts, DMA_BASE + DMA_SNIFF_DATA), ==,
                    0xfffffef5);

    qtest_quit(qts);
}

static void test_dma_error_status(void)
{
    QTestState *qts = rp2040_start();
    uint32_t ctrl = DMA_CTRL_EN | DMA_CTRL_DATA_SIZE_32 |
                    (DREQ_FORCE << DMA_CTRL_TREQ_SEL_SHIFT);

    qtest_writel(qts, SRAM_BASE, 0x12345678);
    qtest_writel(qts, DMA_BASE + DMA_INTR, BIT(0));
    qtest_writel(qts, DMA_BASE + DMA_INTE0, BIT(0));
    qtest_writel(qts, DMA_BASE + DMA_CH_READ_ADDR, BAD_DMA_ADDR);
    qtest_writel(qts, DMA_BASE + DMA_CH_WRITE_ADDR, SRAM_BASE + 4);
    qtest_writel(qts, DMA_BASE + DMA_CH_TRANS_COUNT, 1);
    qtest_writel(qts, DMA_BASE + DMA_CH_CTRL_TRIG, ctrl);

    g_assert_cmphex(qtest_readl(qts, DMA_BASE + DMA_CH_CTRL_TRIG) &
                    (DMA_CTRL_AHB_ERROR | DMA_CTRL_READ_ERROR |
                     DMA_CTRL_WRITE_ERROR | DMA_CTRL_BUSY), ==,
                    DMA_CTRL_AHB_ERROR | DMA_CTRL_READ_ERROR);
    g_assert_cmphex(qtest_readl(qts, DMA_BASE + DMA_CH_TRANS_COUNT), ==, 1);
    g_assert_cmphex(qtest_readl(qts, DMA_BASE + DMA_INTR), ==, BIT(0));
    g_assert_cmphex(qtest_readl(qts, DMA_BASE + DMA_INTS0), ==, BIT(0));

    qtest_writel(qts, DMA_BASE + DMA_INTS0, BIT(0));
    g_assert_cmphex(qtest_readl(qts, DMA_BASE + DMA_INTR), ==, 0);
    g_assert_cmphex(qtest_readl(qts, DMA_BASE + DMA_INTS0), ==, 0);

    qtest_writel(qts, DMA_BASE + DMA_CH_CTRL_TRIG,
                 DMA_CTRL_READ_ERROR | DMA_CTRL_WRITE_ERROR);
    g_assert_cmphex(qtest_readl(qts, DMA_BASE + DMA_CH_CTRL_TRIG) &
                    (DMA_CTRL_AHB_ERROR | DMA_CTRL_READ_ERROR |
                     DMA_CTRL_WRITE_ERROR), ==, 0);

    qtest_writel(qts, DMA_BASE + DMA_CH_READ_ADDR, SRAM_BASE);
    qtest_writel(qts, DMA_BASE + DMA_CH_WRITE_ADDR, BAD_DMA_ADDR);
    qtest_writel(qts, DMA_BASE + DMA_CH_TRANS_COUNT, 1);
    qtest_writel(qts, DMA_BASE + DMA_CH_CTRL_TRIG, ctrl);

    g_assert_cmphex(qtest_readl(qts, DMA_BASE + DMA_CH_CTRL_TRIG) &
                    (DMA_CTRL_AHB_ERROR | DMA_CTRL_READ_ERROR |
                     DMA_CTRL_WRITE_ERROR | DMA_CTRL_BUSY), ==,
                    DMA_CTRL_AHB_ERROR | DMA_CTRL_WRITE_ERROR);
    g_assert_cmphex(qtest_readl(qts, DMA_BASE + DMA_CH_TRANS_COUNT), ==, 1);
    g_assert_cmphex(qtest_readl(qts, DMA_BASE + DMA_INTR), ==, BIT(0));
    g_assert_cmphex(qtest_readl(qts, DMA_BASE + DMA_INTS0), ==, BIT(0));

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/rp2040-dma/xip-ssi-rx-dreq",
                   test_dma_xip_ssi_rx_dreq);
    qtest_add_func("/rp2040-dma/write-ring-wrap",
                   test_dma_write_ring_wrap);
    qtest_add_func("/rp2040-dma/channel-abort",
                   test_dma_channel_abort);
    qtest_add_func("/rp2040-dma/sniff-sum",
                   test_dma_sniff_sum);
    qtest_add_func("/rp2040-dma/error-status",
                   test_dma_error_status);

    return g_test_run();
}
