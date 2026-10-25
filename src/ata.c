#include "ata.h"
#include "vga.h"
#include "kstring.h"
#include <stdint.h>

#define ATA_PRI_DATA        0x1F0
#define ATA_PRI_ERR         0x1F1
#define ATA_PRI_SECCOUNT    0x1F2
#define ATA_PRI_LBA_LO      0x1F3
#define ATA_PRI_LBA_MID     0x1F4
#define ATA_PRI_LBA_HI      0x1F5
#define ATA_PRI_DRIVE       0x1F6
#define ATA_PRI_STATUS      0x1F7
#define ATA_PRI_CMD         0x1F7
#define ATA_PRI_ALT_STATUS  0x3F6
#define ATA_PRI_DEV_CTRL    0x3F6

#define ATA_SR_BSY      0x80
#define ATA_SR_DRDY     0x40
#define ATA_SR_DRQ      0x08
#define ATA_SR_ERR      0x01

#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_FLUSH       0xE7
#define ATA_CMD_IDENTIFY    0xEC

#define ATA_DRIVE_MASTER    0xE0
#define ATA_DRIVE_SLAVE     0xF0

static ata_drive_t drives[2];

static inline void outb(uint16_t p, uint8_t v)  { __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline void outw(uint16_t p, uint16_t v) { __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t  inb(uint16_t p)  { uint8_t  v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint16_t inw(uint16_t p)  { uint16_t v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p)); return v; }

static void ata_delay(void) {
    inb(ATA_PRI_ALT_STATUS); inb(ATA_PRI_ALT_STATUS);
    inb(ATA_PRI_ALT_STATUS); inb(ATA_PRI_ALT_STATUS);
}

static uint8_t ata_poll(int check_drq) {
    ata_delay();
    for (int i = 0; i < 100000; i++) {
        uint8_t st = inb(ATA_PRI_STATUS);
        if (st & ATA_SR_ERR)  return 0xFF;
        if (st & ATA_SR_BSY)  continue;
        if (check_drq && !(st & ATA_SR_DRQ)) continue;
        return st;
    }
    return 0xFF;
}

static int ata_select(int slave) {
    outb(ATA_PRI_DRIVE, slave ? ATA_DRIVE_SLAVE : ATA_DRIVE_MASTER);
    ata_delay();

    for (int i = 0; i < 100000; i++) {
        uint8_t st = inb(ATA_PRI_STATUS);
        if (!(st & ATA_SR_BSY)) return 0;
    }
    return -1;
}

static void ata_fixstr(char *dst, uint16_t *src, int words) {
    for (int i = 0; i < words; i++) {
        dst[i*2]   = (char)(src[i] >> 8);
        dst[i*2+1] = (char)(src[i] & 0xFF);
    }

    int len = words * 2;
    while (len > 0 && dst[len-1] == ' ') len--;
    dst[len] = 0;
}

static int ata_identify(int slave, ata_drive_t *drv) {
    kmemset(drv, 0, sizeof(*drv));

    if (ata_select(slave) < 0) return -1;

    outb(ATA_PRI_SECCOUNT, 0);
    outb(ATA_PRI_LBA_LO,   0);
    outb(ATA_PRI_LBA_MID,  0);
    outb(ATA_PRI_LBA_HI,   0);
    outb(ATA_PRI_CMD, ATA_CMD_IDENTIFY);
    ata_delay();

    uint8_t st = inb(ATA_PRI_STATUS);
    if (st == 0) return -1;

    for (int i = 0; i < 100000; i++) {
        st = inb(ATA_PRI_STATUS);
        if (!(st & ATA_SR_BSY)) break;
        if (i == 99999) return -1;
    }

    if (inb(ATA_PRI_LBA_MID) || inb(ATA_PRI_LBA_HI)) return -1;

    for (int i = 0; i < 100000; i++) {
        st = inb(ATA_PRI_STATUS);
        if (st & ATA_SR_ERR) return -1;
        if (st & ATA_SR_DRQ) break;
        if (i == 99999) return -1;
    }

    uint16_t idata[256];
    for (int i = 0; i < 256; i++)
        idata[i] = inw(ATA_PRI_DATA);

    drv->present  = 1;
    drv->is_slave = slave;
    drv->sectors  = ((uint32_t)idata[61] << 16) | idata[60];

    ata_fixstr(drv->serial, idata + 10, 10);
    ata_fixstr(drv->model,  idata + 27, 20);

    return 0;
}

static void ata_soft_reset(void) {
    outb(ATA_PRI_DEV_CTRL, 0x04);
    ata_delay(); ata_delay();
    outb(ATA_PRI_DEV_CTRL, 0x00);
    ata_delay(); ata_delay();
}

void ata_init(void) {
    for (int attempt = 0; attempt < 3; attempt++) {
        ata_soft_reset();
        if (ata_identify(0, &drives[0]) == 0) break;
    }
    for (int attempt = 0; attempt < 3; attempt++) {
        if (ata_identify(1, &drives[1]) == 0) break;
    }
}

ata_drive_t *ata_get(int drive) {
    if (drive < 0 || drive > 1) return 0;
    return drives[drive].present ? &drives[drive] : 0;
}

int ata_drive_count(void) {
    return (drives[0].present ? 1 : 0) + (drives[1].present ? 1 : 0);
}

int ata_read(int drive, uint32_t lba, uint8_t count, void *buf) {
    if (drive < 0 || drive > 1 || !drives[drive].present) return -1;
    if (!count) return 0;

    int slave = drives[drive].is_slave;

    if (ata_select(slave) < 0) return -1;

    outb(ATA_PRI_DRIVE,    (slave ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F));
    outb(ATA_PRI_ERR,      0x00);
    outb(ATA_PRI_SECCOUNT, count);
    outb(ATA_PRI_LBA_LO,   (uint8_t)(lba & 0xFF));
    outb(ATA_PRI_LBA_MID,  (uint8_t)((lba >> 8)  & 0xFF));
    outb(ATA_PRI_LBA_HI,   (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_PRI_CMD,      ATA_CMD_READ_PIO);

    uint16_t *ptr = (uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        uint8_t st = ata_poll(1);
        if (st == 0xFF) return -1;
        for (int w = 0; w < 256; w++)
            *ptr++ = inw(ATA_PRI_DATA);
    }
    return 0;
}

int ata_write(int drive, uint32_t lba, uint8_t count, const void *buf) {
    if (drive < 0 || drive > 1 || !drives[drive].present) return -1;
    if (!count) return 0;

    int slave = drives[drive].is_slave;

    if (ata_select(slave) < 0) return -1;

    outb(ATA_PRI_DRIVE,    (slave ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F));
    outb(ATA_PRI_ERR,      0x00);
    outb(ATA_PRI_SECCOUNT, count);
    outb(ATA_PRI_LBA_LO,   (uint8_t)(lba & 0xFF));
    outb(ATA_PRI_LBA_MID,  (uint8_t)((lba >> 8)  & 0xFF));
    outb(ATA_PRI_LBA_HI,   (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_PRI_CMD,      ATA_CMD_WRITE_PIO);

    const uint16_t *ptr = (const uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        uint8_t st = ata_poll(1);
        if (st == 0xFF) return -1;
        for (int w = 0; w < 256; w++)
            outw(ATA_PRI_DATA, *ptr++);
    }

    outb(ATA_PRI_CMD, ATA_CMD_FLUSH);
    ata_poll(0);
    return 0;
}

void ata_print_info(void) {
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    vga_puts("\n  === ATA Drives ===\n\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    int found = 0;
    for (int i = 0; i < 2; i++) {
        if (!drives[i].present) continue;
        found = 1;
        vga_puts("  Drive ");
        vga_put_dec(i);
        vga_puts(drives[i].is_slave ? " (slave):  " : " (master): ");
        vga_puts(drives[i].model);
        vga_puts("\n    Serial: ");
        vga_puts(drives[i].serial);
        vga_puts("  Sectors: ");
        vga_put_dec(drives[i].sectors);
        vga_puts(" (");
        vga_put_dec(drives[i].sectors / 2 / 1024);
        vga_puts(" MB)\n");
    }
    if (!found) vga_puts("  No ATA drives found.\n");
    vga_putchar('\n');
}
