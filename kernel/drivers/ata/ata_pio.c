/* drivers/storage/ata_pio.c — ATA PIO block device driver
 *
 * Primary ATA channel:
 *   I/O ports  0x1F0–0x1F7  (data, error/feature, sector count,
 *                             LBA lo/mid/hi, drive/head, status/command)
 *   Control    0x3F6         (alternate status / device control)
 *
 * References:
 *   https://wiki.osdev.org/ATA_PIO_Mode
 *   ATA/ATAPI-6 specification §7
 */
#include "ata_pio.h"
#include "../../log.h"
#include "../../klibc.h"

/* ---- I/O port addresses -------------------------------------------------- */
#define ATA_PRIMARY_BASE   0x1F0
#define ATA_PRIMARY_CTRL   0x3F6

/* Offsets from base */
#define ATA_REG_DATA       0x00   /* 16-bit data port */
#define ATA_REG_ERROR      0x01   /* read: error info */
#define ATA_REG_FEATURE    0x01   /* write: features  */
#define ATA_REG_SECCOUNT   0x02   /* sector count     */
#define ATA_REG_LBA_LO     0x03   /* LBA bits  7:0    */
#define ATA_REG_LBA_MID    0x04   /* LBA bits 15:8    */
#define ATA_REG_LBA_HI     0x05   /* LBA bits 23:16   */
#define ATA_REG_DRIVE_HD   0x06   /* drive/head       */
#define ATA_REG_STATUS     0x07   /* read: status     */
#define ATA_REG_COMMAND    0x07   /* write: command   */

/* Status bits */
#define ATA_SR_BSY   0x80   /* busy               */
#define ATA_SR_DRDY  0x40   /* drive ready        */
#define ATA_SR_DRQ   0x08   /* data request ready */
#define ATA_SR_ERR   0x01   /* error              */

/* Commands */
#define ATA_CMD_READ_PIO   0x20
#define ATA_CMD_WRITE_PIO  0x30
#define ATA_CMD_IDENTIFY   0xEC
#define ATA_CMD_FLUSH      0xE7

/* ---- Port I/O helpers ---------------------------------------------------- */

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0, %1" :: "a"(v), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t v;
    __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outw(uint16_t port, uint16_t v) {
    __asm__ volatile ("outw %0, %1" :: "a"(v), "Nd"(port));
}
static inline void io_wait(void) {
    /* 4 reads of the alternate status port ≈ 400 ns delay */
    inb(ATA_PRIMARY_CTRL);
    inb(ATA_PRIMARY_CTRL);
    inb(ATA_PRIMARY_CTRL);
    inb(ATA_PRIMARY_CTRL);
}

/* ---- Driver state -------------------------------------------------------- */

static int      g_drive_present = 0;
static uint32_t g_sector_count  = 0;

/* ---- Internal helpers ---------------------------------------------------- */

/* Poll until BSY clears; returns status byte or 0xFF on timeout */
static uint8_t ata_wait_bsy(void) {
    for (int i = 0; i < 1000000; i++) {
        uint8_t st = inb(ATA_PRIMARY_BASE + ATA_REG_STATUS);
        if (!(st & ATA_SR_BSY)) return st;
        io_wait();
    }
    kerror("ATA: BSY timeout\n");
    return 0xFF;
}

/* Wait until DRQ is set and BSY is clear */
static int ata_wait_drq(void) {
    for (int i = 0; i < 1000000; i++) {
        uint8_t st = inb(ATA_PRIMARY_BASE + ATA_REG_STATUS);
        if (st & ATA_SR_ERR)  { kerror("ATA: error bit set (st=0x%02x)\n", st); return -1; }
        if (!(st & ATA_SR_BSY) && (st & ATA_SR_DRQ)) return 0;
        io_wait();
    }
    kerror("ATA: DRQ timeout\n");
    return -1;
}

/* Select master drive on primary channel and set up 28-bit LBA */
static void ata_select_lba28(uint32_t lba, uint8_t count) {
    /* Drive/Head: bit7=1 bit6=1(LBA) bit5=1 bit4=0(master) bits3:0=LBA[27:24] */
    outb(ATA_PRIMARY_BASE + ATA_REG_DRIVE_HD,
         (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    io_wait();
    outb(ATA_PRIMARY_BASE + ATA_REG_FEATURE,  0x00);
    outb(ATA_PRIMARY_BASE + ATA_REG_SECCOUNT, count);
    outb(ATA_PRIMARY_BASE + ATA_REG_LBA_LO,   (uint8_t)(lba & 0xFF));
    outb(ATA_PRIMARY_BASE + ATA_REG_LBA_MID,  (uint8_t)((lba >> 8)  & 0xFF));
    outb(ATA_PRIMARY_BASE + ATA_REG_LBA_HI,   (uint8_t)((lba >> 16) & 0xFF));
}

/* ---- Public API ---------------------------------------------------------- */

int ata_pio_init(void) {
    /* Software reset: clear nIEN and SRST bits */
    outb(ATA_PRIMARY_CTRL, 0x00);
    io_wait();

    /* Select master */
    outb(ATA_PRIMARY_BASE + ATA_REG_DRIVE_HD, 0xA0);
    io_wait();

    /* Send IDENTIFY command */
    outb(ATA_PRIMARY_BASE + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    io_wait();

    /* If status is 0, no drive present */
    uint8_t st = inb(ATA_PRIMARY_BASE + ATA_REG_STATUS);
    if (st == 0x00) {
        kinfo("ATA: no drive on primary master\n");
        g_drive_present = 0;
        return 0;
    }

    /* Wait for BSY to clear */
    st = ata_wait_bsy();
    if (st == 0xFF) { g_drive_present = 0; return 0; }

    /* Check for ATAPI (non-ATA) — LBA_MID/HI non-zero after IDENTIFY */
    if (inb(ATA_PRIMARY_BASE + ATA_REG_LBA_MID) != 0 ||
        inb(ATA_PRIMARY_BASE + ATA_REG_LBA_HI)  != 0) {
        kinfo("ATA: ATAPI device detected (not supported in PIO mode)\n");
        g_drive_present = 0;
        return 0;
    }

    /* Wait for DRQ */
    if (ata_wait_drq() < 0) { g_drive_present = 0; return 0; }

    /* Read 256 words of IDENTIFY data */
    uint16_t id[256];
    for (int i = 0; i < 256; i++)
        id[i] = inw(ATA_PRIMARY_BASE + ATA_REG_DATA);

    /* Word 60+61: 28-bit LBA sector count */
    g_sector_count = ((uint32_t)id[61] << 16) | id[60];
    g_drive_present = 1;

    kinfo("ATA: primary master detected — %u sectors (%u MB)\n",
          g_sector_count,
          (uint32_t)((uint64_t)g_sector_count * 512 / (1024 * 1024)));
    return 1;
}

int ata_pio_present(void)          { return g_drive_present; }
uint32_t ata_pio_sector_count(void){ return g_sector_count;  }

int ata_pio_read(uint32_t lba, uint32_t count, void *buf) {
    if (!g_drive_present)        return -1;
    if (!count || !buf)          return -1;
    if (lba + count > g_sector_count) return -1;

    uint16_t *dst = (uint16_t *)buf;

    for (uint32_t s = 0; s < count; s++) {
        ata_wait_bsy();
        ata_select_lba28(lba + s, 1);
        outb(ATA_PRIMARY_BASE + ATA_REG_COMMAND, ATA_CMD_READ_PIO);
        io_wait();

        if (ata_wait_drq() < 0) return -1;

        for (int w = 0; w < 256; w++)
            dst[s * 256 + w] = inw(ATA_PRIMARY_BASE + ATA_REG_DATA);

        /* Four ALT_STATUS reads after each sector (recommended by spec) */
        io_wait();
    }
    return 0;
}

int ata_pio_write(uint32_t lba, uint32_t count, const void *buf) {
    if (!g_drive_present)        return -1;
    if (!count || !buf)          return -1;
    if (lba + count > g_sector_count) return -1;

    const uint16_t *src = (const uint16_t *)buf;

    for (uint32_t s = 0; s < count; s++) {
        ata_wait_bsy();
        ata_select_lba28(lba + s, 1);
        outb(ATA_PRIMARY_BASE + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);
        io_wait();

        if (ata_wait_drq() < 0) return -1;

        for (int w = 0; w < 256; w++)
            outw(ATA_PRIMARY_BASE + ATA_REG_DATA, src[s * 256 + w]);

        io_wait();
        /* Flush write cache */
        outb(ATA_PRIMARY_BASE + ATA_REG_COMMAND, ATA_CMD_FLUSH);
        ata_wait_bsy();
    }
    return 0;
}
