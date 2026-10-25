#ifndef FAT12_H
#define FAT12_H

#include <stdint.h>

#define FAT12_MAX_FILES     224
#define FAT12_NAME_LEN      9
#define FAT12_EXT_LEN       4
#define FAT12_SECTOR_SIZE   512

typedef struct {
    char     name[13];
    uint32_t size;
    uint16_t start_cluster;
    uint8_t  attr;
    int      is_dir;
} fat12_entry_t;

int  fat12_mount(int ata_drive);
int  fat12_mounted(void);

int  fat12_list(fat12_entry_t *entries, int max);

int  fat12_read(const char *name, void *buf, uint32_t bufsz);

int  fat12_write(const char *name, const void *buf, uint32_t size);

int  fat12_delete(const char *name);
int  fat12_rename(const char *old_name, const char *new_name);

int  fat12_set_readonly(const char *name, int ro);
int  fat12_is_readonly(const char *name);

int  fat12_format(int ata_drive, const char *label);

void fat12_info(void);
int  fat12_space(uint32_t *total_kb, uint32_t *free_kb);

#endif
