#ifndef FS_H
#define FS_H

#include <stddef.h>
#include <stdint.h>

#define FS_MAX_FILES    32
#define FS_MAX_NAME     64
#define FS_MAX_DATA   4096

typedef struct {
    char     name[FS_MAX_NAME];
    char     data[FS_MAX_DATA];
    uint32_t size;
    int      used;
    int      executable;
} fs_file_t;

void       fs_init(void);
int        fs_create(const char *name, const char *data, int exec);
int        fs_write(const char *name, const char *data);
fs_file_t *fs_find(const char *name);
int        fs_delete(const char *name);
void       fs_list(void);
int        fs_file_count(void);
int        fs_list_names(char *buf, uint32_t sz);

#endif