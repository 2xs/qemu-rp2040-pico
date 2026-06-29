/*
 * RP2040 XIP/SSI flash controller emulation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "elf.h"
#include "exec/memattrs.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/loader.h"
#include "hw/ssi/rp2040_xip.h"
#include "qemu/log.h"

#define RP2040_XIP_CTRL_EN           0x1
#define RP2040_XIP_CTRL_ERR_BADWRITE 0x2
#define RP2040_XIP_STAT_FLUSH_READY  0x1
#define RP2040_XIP_STAT_FIFO_EMPTY   0x2
#define RP2040_XIP_FLASH_BASE        0x10000000

#define RP2040_SSI_CTRLR0     0x00
#define RP2040_SSI_CTRLR1     0x04
#define RP2040_SSI_SSIENR     0x08
#define RP2040_SSI_SER        0x10
#define RP2040_SSI_BAUDR      0x14
#define RP2040_SSI_TXFTLR     0x18
#define RP2040_SSI_RXFTLR     0x1c
#define RP2040_SSI_TXFLR      0x20
#define RP2040_SSI_RXFLR      0x24
#define RP2040_SSI_SR         0x28
#define RP2040_SSI_IMR        0x2c
#define RP2040_SSI_ISR        0x30
#define RP2040_SSI_RISR       0x34
#define RP2040_SSI_DMACR      0x4c
#define RP2040_SSI_DMATDLR    0x50
#define RP2040_SSI_DMARDLR    0x54
#define RP2040_SSI_IDR        0x58
#define RP2040_SSI_VERSION_ID 0x5c
#define RP2040_SSI_DR0        0x60
#define RP2040_SSI_DR_END     0xec
#define RP2040_SSI_SPI_CTRLR0 0xf4

#define RP2040_SSI_SR_BUSY 0x01
#define RP2040_SSI_SR_TFNF 0x02
#define RP2040_SSI_SR_TFE  0x04
#define RP2040_SSI_SR_RFNE 0x08
#define RP2040_SSI_SR_RFF  0x10

#define FLASH_CMD_READ         0x03
#define FLASH_CMD_PAGE_PROGRAM 0x02
#define FLASH_CMD_READ_STATUS  0x05
#define FLASH_CMD_WRITE_ENABLE 0x06
#define FLASH_CMD_SECTOR_ERASE 0x20

#define FLASH_STATUS_WIP 0x01
#define FLASH_STATUS_WEL 0x02
#define FLASH_PAGE_SIZE  256
#define FLASH_SECTOR_SIZE 4096

#define UF2_MAGIC_START0 0x0a324655
#define UF2_MAGIC_START1 0x9e5d5157
#define UF2_MAGIC_END    0x0ab16f30
#define UF2_BLOCK_SIZE   512
#define UF2_HEADER_SIZE  32
#define UF2_MAX_PAYLOAD  476

#define UF2_FLAG_NOT_MAIN_FLASH       BIT(0)
#define UF2_FLAG_FAMILY_ID_PRESENT    BIT(13)
#define UF2_RP2040_FAMILY_ID          0xe48bff56

#define ATOMIC_ALIAS_MASK 0x3000
#define ATOMIC_XOR        0x1000
#define ATOMIC_SET        0x2000
#define ATOMIC_CLR        0x3000

static uint32_t rp2040_xip_apply_alias(uint32_t old, uint32_t value,
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

static void rp2040_xip_rx_clear(RP2040XipState *s)
{
    s->rx_len = 0;
    s->rx_pos = 0;
}

static void rp2040_xip_rx_push(RP2040XipState *s, uint8_t value)
{
    if (s->rx_len < ARRAY_SIZE(s->rx)) {
        s->rx[s->rx_len++] = value;
    }
}

static uint8_t rp2040_xip_status(RP2040XipState *s)
{
    uint8_t status = 0;

    if (s->busy) {
        status |= FLASH_STATUS_WIP;
    }
    if (s->write_enable) {
        status |= FLASH_STATUS_WEL;
    }

    return status;
}

static uint32_t rp2040_xip_tx_addr(RP2040XipState *s)
{
    return (uint32_t)s->tx[1] << 16 | s->tx[2] << 8 | s->tx[3];
}

static void rp2040_xip_finish_busy(RP2040XipState *s)
{
    s->busy = false;
}

static void rp2040_xip_reset_tx(RP2040XipState *s)
{
    s->tx_len = 0;
}

static void rp2040_xip_program(RP2040XipState *s)
{
    uint32_t addr;
    uint32_t page_end;
    unsigned data_len;
    unsigned i;

    if (!s->write_enable) {
        return;
    }

    s->write_enable = false;

    if (s->tx_len <= 4) {
        return;
    }

    addr = rp2040_xip_tx_addr(s);
    if (addr >= s->flash_size) {
        return;
    }

    page_end = ROUND_UP(addr + 1, FLASH_PAGE_SIZE);
    data_len = MIN(s->tx_len - 4, page_end - addr);
    data_len = MIN(data_len, s->flash_size - addr);

    for (i = 0; i < data_len; i++) {
        s->storage[addr + i] &= s->tx[4 + i];
    }

    s->busy = true;
}

static void rp2040_xip_erase(RP2040XipState *s)
{
    uint32_t addr;
    uint32_t base;

    if (!s->write_enable) {
        return;
    }

    s->write_enable = false;

    if (s->tx_len < 4) {
        return;
    }

    addr = rp2040_xip_tx_addr(s);
    base = QEMU_ALIGN_DOWN(addr, FLASH_SECTOR_SIZE);
    if (base >= s->flash_size) {
        return;
    }

    memset(&s->storage[base], 0xff, MIN(FLASH_SECTOR_SIZE,
                                       s->flash_size - base));
    s->busy = true;
}

static void rp2040_xip_finish_command(RP2040XipState *s)
{
    if (s->tx_len == 0) {
        return;
    }

    switch (s->tx[0]) {
    case FLASH_CMD_PAGE_PROGRAM:
        rp2040_xip_program(s);
        break;
    case FLASH_CMD_SECTOR_ERASE:
        rp2040_xip_erase(s);
        break;
    default:
        break;
    }

    rp2040_xip_reset_tx(s);
}

static void rp2040_xip_dr_write(RP2040XipState *s, uint8_t value)
{
    uint32_t addr;

    if (s->tx_len < ARRAY_SIZE(s->tx)) {
        s->tx[s->tx_len++] = value;
    }

    switch (s->tx[0]) {
    case FLASH_CMD_WRITE_ENABLE:
        s->write_enable = true;
        rp2040_xip_reset_tx(s);
        break;
    case FLASH_CMD_READ_STATUS:
        rp2040_xip_rx_push(s, rp2040_xip_status(s));
        rp2040_xip_finish_busy(s);
        rp2040_xip_reset_tx(s);
        break;
    case FLASH_CMD_READ:
        if (s->tx_len >= 4) {
            addr = rp2040_xip_tx_addr(s) + s->tx_len - 4;
            rp2040_xip_rx_push(s, addr < s->flash_size ?
                               s->storage[addr] : 0xff);
        }
        break;
    default:
        break;
    }
}

static MemTxResult rp2040_xip_read(void *opaque, hwaddr addr, uint64_t *data,
                                   unsigned size, MemTxAttrs attrs)
{
    RP2040XipState *s = opaque;
    uint64_t value = 0;
    unsigned i;

    if (s->busy || addr + size > s->flash_size) {
        return MEMTX_ERROR;
    }

    for (i = 0; i < size; i++) {
        value |= (uint64_t)s->storage[addr + i] << (i * 8);
    }
    *data = value;
    return MEMTX_OK;
}

static MemTxResult rp2040_xip_write(void *opaque, hwaddr addr, uint64_t data,
                                    unsigned size, MemTxAttrs attrs)
{
    RP2040XipState *s = opaque;
    unsigned i;

    if (!s->xip_writable) {
        return MEMTX_ERROR;
    }
    if (addr + size > s->flash_size) {
        return MEMTX_ERROR;
    }

    for (i = 0; i < size; i++) {
        s->storage[addr + i] = extract64(data, i * 8, 8);
    }
    return MEMTX_OK;
}

static uint64_t rp2040_xip_ctrl_read(void *opaque, hwaddr addr, unsigned size)
{
    RP2040XipState *s = opaque;
    hwaddr offset = addr & 0xfff;
    uint64_t value;

    switch (offset) {
    case 0x00:
        value = s->xip_ctrl;
        break;
    case 0x04:
        value = 0;
        break;
    case 0x08:
        value = RP2040_XIP_STAT_FLUSH_READY | RP2040_XIP_STAT_FIFO_EMPTY;
        break;
    default:
        value = 0;
        break;
    }

    qemu_log_mask(LOG_UNIMP, "rp2040.xip.ctrl: read  "
                  "(size %d, addr 0x%08" HWADDR_PRIx
                  ", offset 0x%04" HWADDR_PRIx
                  ") -> 0x%0*" PRIx64 "\n",
                  size, RP2040_XIP_CTRL_BASE + addr, offset,
                  size << 1, value);

    return value;
}

static void rp2040_xip_ctrl_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    RP2040XipState *s = opaque;
    hwaddr alias = addr & ATOMIC_ALIAS_MASK;
    hwaddr offset = addr & 0xfff;
    uint32_t new_value;

    switch (offset) {
    case 0x00:
        new_value = rp2040_xip_apply_alias(s->xip_ctrl, value, alias);
        s->xip_ctrl = new_value & (RP2040_XIP_CTRL_EN |
                                   RP2040_XIP_CTRL_ERR_BADWRITE);
        break;
    case 0x0c:
    case 0x10:
        break;
    default:
        break;
    }

    qemu_log_mask(LOG_UNIMP, "rp2040.xip.ctrl: write "
                  "(size %d, addr 0x%08" HWADDR_PRIx
                  ", offset 0x%04" HWADDR_PRIx
                  ", value 0x%0*" PRIx64 ")\n",
                  size, RP2040_XIP_CTRL_BASE + addr, offset,
                  size << 1, value);
}

static uint64_t rp2040_xip_ssi_read(void *opaque, hwaddr addr, unsigned size)
{
    RP2040XipState *s = opaque;
    hwaddr offset = addr & 0xfff;
    uint8_t value;
    uint32_t risr = s->rx_len > s->rx_pos ? 0 : 1;
    uint64_t ret;

    if (offset >= RP2040_SSI_DR0 && offset <= RP2040_SSI_DR_END) {
        if (s->rx_pos < s->rx_len) {
            value = s->rx[s->rx_pos++];
            if (s->rx_pos == s->rx_len) {
                rp2040_xip_rx_clear(s);
            }
            ret = value;
        } else {
            ret = 0;
        }
        qemu_log_mask(LOG_UNIMP, "rp2040.xip.ssi: read  "
                      "(size %d, addr 0x%08" HWADDR_PRIx
                      ", offset 0x%04" HWADDR_PRIx
                      ") -> 0x%0*" PRIx64 "\n",
                      size, RP2040_XIP_SSI_BASE + addr, offset,
                      size << 1, ret);
        return ret;
    }

    switch (offset) {
    case RP2040_SSI_CTRLR0:
        ret = s->ctrlr0;
        break;
    case RP2040_SSI_CTRLR1:
        ret = s->ctrlr1;
        break;
    case RP2040_SSI_SSIENR:
        ret = s->ssienr;
        break;
    case RP2040_SSI_SER:
        ret = s->ser;
        break;
    case RP2040_SSI_BAUDR:
        ret = s->baudr;
        break;
    case RP2040_SSI_TXFTLR:
        ret = s->txftlr;
        break;
    case RP2040_SSI_RXFTLR:
        ret = s->rxftlr;
        break;
    case RP2040_SSI_TXFLR:
        ret = 0;
        break;
    case RP2040_SSI_RXFLR:
        ret = s->rx_len - s->rx_pos;
        break;
    case RP2040_SSI_SR:
        ret = RP2040_SSI_SR_TFE | RP2040_SSI_SR_TFNF |
              (s->busy ? RP2040_SSI_SR_BUSY : 0) |
              (s->rx_len > s->rx_pos ? RP2040_SSI_SR_RFNE : 0) |
              (s->rx_len - s->rx_pos == ARRAY_SIZE(s->rx) ?
               RP2040_SSI_SR_RFF : 0);
        break;
    case RP2040_SSI_IMR:
        ret = s->imr;
        break;
    case RP2040_SSI_ISR:
    case RP2040_SSI_RISR:
        ret = risr;
        break;
    case RP2040_SSI_DMACR:
    case RP2040_SSI_DMATDLR:
        ret = 0;
        break;
    case RP2040_SSI_DMARDLR:
        ret = 4;
        break;
    case RP2040_SSI_IDR:
        ret = 0;
        break;
    case RP2040_SSI_VERSION_ID:
        ret = 0x3430312a;
        break;
    case RP2040_SSI_SPI_CTRLR0:
        ret = s->spi_ctrlr0;
        break;
    default:
        ret = 0;
        break;
    }

    qemu_log_mask(LOG_UNIMP, "rp2040.xip.ssi: read  "
                  "(size %d, addr 0x%08" HWADDR_PRIx
                  ", offset 0x%04" HWADDR_PRIx
                  ") -> 0x%0*" PRIx64 "\n",
                  size, RP2040_XIP_SSI_BASE + addr, offset,
                  size << 1, ret);

    return ret;
}

static void rp2040_xip_ssi_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned size)
{
    RP2040XipState *s = opaque;
    hwaddr alias = addr & ATOMIC_ALIAS_MASK;
    hwaddr offset = addr & 0xfff;
    uint32_t old_ser = s->ser;
    uint32_t new_value;

    if (offset >= RP2040_SSI_DR0 && offset <= RP2040_SSI_DR_END) {
        rp2040_xip_dr_write(s, value & 0xff);
        qemu_log_mask(LOG_UNIMP, "rp2040.xip.ssi: write "
                      "(size %d, addr 0x%08" HWADDR_PRIx
                      ", offset 0x%04" HWADDR_PRIx
                      ", value 0x%0*" PRIx64 ")\n",
                      size, RP2040_XIP_SSI_BASE + addr, offset,
                      size << 1, value);
        return;
    }

    switch (offset) {
    case RP2040_SSI_CTRLR0:
        s->ctrlr0 = rp2040_xip_apply_alias(s->ctrlr0, value, alias);
        break;
    case RP2040_SSI_CTRLR1:
        s->ctrlr1 = rp2040_xip_apply_alias(s->ctrlr1, value, alias);
        break;
    case RP2040_SSI_SSIENR:
        new_value = rp2040_xip_apply_alias(s->ssienr, value, alias);
        s->ssienr = new_value & 1;
        if (!s->ssienr) {
            rp2040_xip_rx_clear(s);
            rp2040_xip_reset_tx(s);
        }
        break;
    case RP2040_SSI_SER:
        new_value = rp2040_xip_apply_alias(s->ser, value, alias);
        s->ser = new_value & 1;
        if ((old_ser & 1) && !s->ser) {
            rp2040_xip_finish_command(s);
        }
        break;
    case RP2040_SSI_BAUDR:
        new_value = rp2040_xip_apply_alias(s->baudr, value, alias);
        s->baudr = new_value & 0xffff;
        break;
    case RP2040_SSI_TXFTLR:
        new_value = rp2040_xip_apply_alias(s->txftlr, value, alias);
        s->txftlr = new_value & 0xff;
        break;
    case RP2040_SSI_RXFTLR:
        new_value = rp2040_xip_apply_alias(s->rxftlr, value, alias);
        s->rxftlr = new_value & 0xff;
        break;
    case RP2040_SSI_IMR:
        new_value = rp2040_xip_apply_alias(s->imr, value, alias);
        s->imr = new_value & 0x3f;
        break;
    case RP2040_SSI_SPI_CTRLR0:
        s->spi_ctrlr0 = rp2040_xip_apply_alias(s->spi_ctrlr0, value, alias);
        break;
    default:
        break;
    }

    qemu_log_mask(LOG_UNIMP, "rp2040.xip.ssi: write "
                  "(size %d, addr 0x%08" HWADDR_PRIx
                  ", offset 0x%04" HWADDR_PRIx
                  ", value 0x%0*" PRIx64 ")\n",
                  size, RP2040_XIP_SSI_BASE + addr, offset,
                  size << 1, value);
}

static const MemoryRegionOps rp2040_xip_ops = {
    .read_with_attrs = rp2040_xip_read,
    .write_with_attrs = rp2040_xip_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
};

static const MemoryRegionOps rp2040_xip_ctrl_ops = {
    .read = rp2040_xip_ctrl_read,
    .write = rp2040_xip_ctrl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static const MemoryRegionOps rp2040_xip_ssi_ops = {
    .read = rp2040_xip_ssi_read,
    .write = rp2040_xip_ssi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
};

void rp2040_xip_set_writable(RP2040XipState *s, bool writable)
{
    s->xip_writable = writable;
}

static bool rp2040_xip_load_elf(RP2040XipState *s, const char *filename,
                                Error **errp)
{
    g_autofree gchar *contents = NULL;
    gsize len;
    const Elf32_Ehdr *ehdr;
    const Elf32_Phdr *phdr;
    int i;

    if (!g_file_get_contents(filename, &contents, &len, NULL)) {
        error_setg(errp, "could not load flash image '%s'", filename);
        return true;
    }

    if (len < sizeof(*ehdr)) {
        return false;
    }

    ehdr = (const Elf32_Ehdr *)contents;
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        return false;
    }
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS32 ||
        ehdr->e_ident[EI_DATA] != ELFDATA2LSB ||
        le16_to_cpu(ehdr->e_machine) != EM_ARM) {
        error_setg(errp, "unsupported flash ELF image '%s'", filename);
        return true;
    }
    if (le32_to_cpu(ehdr->e_phoff) > len ||
        le16_to_cpu(ehdr->e_phentsize) != sizeof(*phdr) ||
        le16_to_cpu(ehdr->e_phnum) >
        (len - le32_to_cpu(ehdr->e_phoff)) / sizeof(*phdr)) {
        error_setg(errp, "invalid flash ELF image '%s'", filename);
        return true;
    }

    phdr = (const Elf32_Phdr *)(contents + le32_to_cpu(ehdr->e_phoff));
    for (i = 0; i < le16_to_cpu(ehdr->e_phnum); i++) {
        uint32_t paddr = le32_to_cpu(phdr[i].p_paddr);
        uint32_t filesz = le32_to_cpu(phdr[i].p_filesz);
        uint32_t memsz = le32_to_cpu(phdr[i].p_memsz);
        uint32_t off = le32_to_cpu(phdr[i].p_offset);
        uint32_t xip_off;

        if (le32_to_cpu(phdr[i].p_type) != PT_LOAD) {
            continue;
        }

        if (paddr < RP2040_XIP_FLASH_BASE ||
            paddr - RP2040_XIP_FLASH_BASE > s->flash_size ||
            filesz > memsz ||
            memsz > s->flash_size - (paddr - RP2040_XIP_FLASH_BASE) ||
            off > len ||
            filesz > len - off) {
            error_setg(errp, "flash ELF segment is outside XIP storage");
            return true;
        }

        xip_off = paddr - RP2040_XIP_FLASH_BASE;
        memcpy(s->storage + xip_off, contents + off, filesz);
        if (memsz > filesz) {
            memset(s->storage + xip_off + filesz, 0, memsz - filesz);
        }
    }

    return true;
}

static bool rp2040_xip_load_uf2(RP2040XipState *s, const char *filename,
                                Error **errp)
{
    g_autofree gchar *contents = NULL;
    gsize len;
    unsigned blocks;
    unsigned i;
    bool copied = false;

    if (!g_file_get_contents(filename, &contents, &len, NULL)) {
        error_setg(errp, "could not load flash image '%s'", filename);
        return true;
    }

    if (len < UF2_BLOCK_SIZE ||
        ldl_le_p(contents) != UF2_MAGIC_START0 ||
        ldl_le_p(contents + 4) != UF2_MAGIC_START1) {
        return false;
    }

    if (len % UF2_BLOCK_SIZE != 0) {
        error_setg(errp, "invalid UF2 image '%s': size is not a multiple "
                   "of %u bytes", filename, UF2_BLOCK_SIZE);
        return true;
    }

    blocks = len / UF2_BLOCK_SIZE;
    for (i = 0; i < blocks; i++) {
        const uint8_t *block = (uint8_t *)contents + i * UF2_BLOCK_SIZE;
        uint32_t flags;
        uint32_t target;
        uint32_t payload_size;
        uint32_t family_id;
        uint32_t offset;

        if (ldl_le_p(block) != UF2_MAGIC_START0 ||
            ldl_le_p(block + 4) != UF2_MAGIC_START1 ||
            ldl_le_p(block + UF2_BLOCK_SIZE - 4) != UF2_MAGIC_END) {
            error_setg(errp, "invalid UF2 image '%s': bad magic in block %u",
                       filename, i);
            return true;
        }

        flags = ldl_le_p(block + 8);
        if (flags & UF2_FLAG_NOT_MAIN_FLASH) {
            continue;
        }

        if (!(flags & UF2_FLAG_FAMILY_ID_PRESENT)) {
            error_setg(errp, "invalid RP2040 UF2 image '%s': block %u has "
                       "no family ID", filename, i);
            return true;
        }

        family_id = ldl_le_p(block + 28);
        if (family_id != UF2_RP2040_FAMILY_ID) {
            error_setg(errp, "unsupported UF2 family ID 0x%08" PRIx32
                       " in '%s'", family_id, filename);
            return true;
        }

        target = ldl_le_p(block + 12);
        payload_size = ldl_le_p(block + 16);
        if (payload_size > UF2_MAX_PAYLOAD ||
            target < RP2040_XIP_FLASH_BASE ||
            target - RP2040_XIP_FLASH_BASE > s->flash_size ||
            payload_size > s->flash_size -
                           (target - RP2040_XIP_FLASH_BASE)) {
            error_setg(errp, "UF2 block %u in '%s' is outside XIP flash",
                       i, filename);
            return true;
        }

        offset = target - RP2040_XIP_FLASH_BASE;
        memcpy(s->storage + offset, block + UF2_HEADER_SIZE, payload_size);
        copied = true;
    }

    if (!copied) {
        error_setg(errp, "UF2 image '%s' contains no RP2040 flash payload",
                   filename);
    }

    return true;
}

void rp2040_xip_load_image(RP2040XipState *s, const char *filename,
                           Error **errp)
{
    ssize_t image_size;

    if (!filename) {
        return;
    }

    if (rp2040_xip_load_elf(s, filename, errp)) {
        return;
    }

    if (rp2040_xip_load_uf2(s, filename, errp)) {
        return;
    }

    image_size = load_image_size(filename, s->storage, s->flash_size);
    if (image_size < 0) {
        error_setg(errp, "could not load flash image '%s'", filename);
    }
}

static void rp2040_xip_realize(DeviceState *dev, Error **errp)
{
    RP2040XipState *s = RP2040_XIP(dev);
    g_autofree gchar *contents = NULL;
    gsize contents_len = 0;

    if (s->flash_size == 0) {
        error_setg(errp, "flash-size must be non-zero");
        return;
    }

    s->xip_writable = true;
    s->storage = g_malloc0(s->flash_size);
    memset(s->storage, 0xff, s->flash_size);

    if (s->flash_file) {
        if (!g_file_get_contents(s->flash_file, &contents, &contents_len,
                                 NULL)) {
            error_setg(errp, "could not load flash file '%s'",
                       s->flash_file);
            return;
        }
        if (contents_len > s->flash_size) {
            error_setg(errp, "flash file '%s' is %" G_GSIZE_FORMAT
                       " bytes, larger than %" G_GSIZE_FORMAT
                       " byte Pico flash",
                       s->flash_file, contents_len, (gsize)s->flash_size);
            return;
        }
        memcpy(s->storage, contents, contents_len);
    }

    memory_region_init_io(&s->xip, OBJECT(dev), &rp2040_xip_ops, s,
                          "rp2040.xip", s->flash_size);
    memory_region_init_io(&s->ctrl, OBJECT(dev), &rp2040_xip_ctrl_ops, s,
                          "rp2040.xip.ctrl", RP2040_XIP_CTRL_SIZE);
    memory_region_init_io(&s->ssi, OBJECT(dev), &rp2040_xip_ssi_ops, s,
                          "rp2040.xip.ssi", RP2040_XIP_SSI_SIZE);

    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->xip);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->ctrl);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->ssi);
}

static void rp2040_xip_reset(DeviceState *dev)
{
    RP2040XipState *s = RP2040_XIP(dev);

    s->xip_ctrl = RP2040_XIP_CTRL_EN | RP2040_XIP_CTRL_ERR_BADWRITE;
    s->ctrlr0 = 0;
    s->ctrlr1 = 0;
    s->ssienr = 0;
    s->ser = 0;
    s->baudr = 0;
    s->txftlr = 0;
    s->rxftlr = 0;
    s->imr = 0;
    s->spi_ctrlr0 = 0;
    s->write_enable = false;
    s->busy = false;
    rp2040_xip_reset_tx(s);
    rp2040_xip_rx_clear(s);
}

static void rp2040_xip_finalize(Object *obj)
{
    RP2040XipState *s = RP2040_XIP(obj);

    g_free(s->flash_file);
    g_free(s->storage);
}

static const Property rp2040_xip_properties[] = {
    DEFINE_PROP_UINT32("flash-size", RP2040XipState, flash_size, 2 * MiB),
    DEFINE_PROP_STRING("flash-file", RP2040XipState, flash_file),
};

static void rp2040_xip_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rp2040_xip_realize;
    device_class_set_legacy_reset(dc, rp2040_xip_reset);
    device_class_set_props(dc, rp2040_xip_properties);
}

static const TypeInfo rp2040_xip_info = {
    .name          = TYPE_RP2040_XIP,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RP2040XipState),
    .instance_finalize = rp2040_xip_finalize,
    .class_init    = rp2040_xip_class_init,
};

static void rp2040_xip_register_types(void)
{
    type_register_static(&rp2040_xip_info);
}
type_init(rp2040_xip_register_types)
