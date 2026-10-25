#ifndef EXT2_H
#define EXT2_H
#include <stdint.h>
#include "vfs.h"

#define EXT2_MAGIC       0xEF53
#define EXT2_ROOT_INO    2
#define EXT2_FT_REG      1
#define EXT2_FT_DIR      2

typedef struct {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime, s_wtime;
    uint16_t s_mnt_count, s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck, s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid, s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint8_t  pad[934];
} __attribute__((packed)) ext2_super_t;

typedef struct {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} __attribute__((packed)) ext2_bgd_t;

typedef struct {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime, i_ctime, i_mtime, i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} __attribute__((packed)) ext2_inode_t;

typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[256];
} __attribute__((packed)) ext2_dirent_t;

int  ext2_init(int drive, uint32_t lba_start);
int  ext2_format(int drive, uint32_t lba_start);
int  ext2_mounted(void);

int  ext2_read_file(const char *path, void *buf, uint32_t sz);
int  ext2_write_file(const char *path, const void *buf, uint32_t size);
int  ext2_delete_file(const char *path);
int  ext2_list_dir(const char *path, char *buf, uint32_t sz);
int  ext2_stat(const char *path, uint32_t *size_out, int *is_dir);
void ext2_info(void);
int  ext2_space(uint32_t *total_kb, uint32_t *free_kb);

extern vfs_ops_t ext2_vfs_ops;
#endif
