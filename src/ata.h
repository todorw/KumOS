#ifndef ATA_H
#define ATA_H

#include <stdint.h>

typedef struct {
    int      present;
    int      is_slave;
    uint32_t sectors;
    char     model[41];
    char     serial[21];
} ata_drive_t;

void          ata_init(void);
ata_drive_t  *ata_get(int drive);
int           ata_drive_count(void);

int  ata_read (int drive, uint32_t lba, uint8_t count, void *buf);
int  ata_write(int drive, uint32_t lba, uint8_t count, const void *buf);

void ata_print_info(void);

#endif
