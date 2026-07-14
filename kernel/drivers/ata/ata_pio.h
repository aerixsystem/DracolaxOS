/* drivers/storage/ata_pio.h — ATA PIO block device driver
 *
 * Implements 28-bit LBA PIO reads and writes for the primary IDE channel.
 * Works with QEMU's default IDE controller (-drive file=disk.img,format=raw)
 * and real hardware IDE controllers.
 *
 * Limitations (v1, audit item 3.5):
 *   - Primary channel only (I/O base 0x1F0, control 0x3F6)
 *   - Master drive only (Drive/Head bit 4 = 0)
 *   - 28-bit LBA (max ~128 GB)
 *   - PIO mode (no DMA) — transfers via INS/OUTS instructions
 *   - Blocking — polls BSY until drive is ready
 *
 * A block is 512 bytes (one ATA sector).
 */
#ifndef ATA_PIO_H
#define ATA_PIO_H
#include "../../types.h"

/* Block size in bytes */
#define ATA_SECTOR_SIZE  512

/* Initialise the ATA PIO driver.
 * Detects whether a drive is present on the primary master channel.
 * Returns 1 if a drive was found, 0 if no drive (e.g. pure-RAM VM). */
int  ata_pio_init(void);

/* Returns 1 if a drive is present (ata_pio_init returned 1). */
int  ata_pio_present(void);

/* Read count 512-byte sectors starting at LBA lba into buf.
 * Returns 0 on success, -1 on error. */
int  ata_pio_read(uint32_t lba, uint32_t count, void *buf);

/* Write count 512-byte sectors from buf to LBA lba.
 * Returns 0 on success, -1 on error. */
int  ata_pio_write(uint32_t lba, uint32_t count, const void *buf);

/* Return the drive's sector count (0 if no drive). */
uint32_t ata_pio_sector_count(void);

#endif /* ATA_PIO_H */
