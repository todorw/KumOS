#include "ext2.h"
#include "ata.h"
#include "kstring.h"
#include "vfs.h"
#include "vga.h"
#include "kmalloc.h"
#include "irq.h"
#include <stdint.h>

#define EXT2_BITMAP_BITS 8192
#define EXT2_WBUF_MAX    (64u * 1024u)

#define EXT2_FMT_BLOCKS  8192
#define EXT2_FMT_INODES  1024

static ext2_super_t sb;
static ext2_bgd_t   bgd;
static uint32_t     block_size = 1024;
static uint32_t     lba_base   = 0;
static int          g_drive    = 0;
static int          mounted    = 0;

static void read_block(uint32_t blk, void *buf) {
    uint32_t sectors_per_block = block_size / 512;
    uint32_t lba = lba_base + blk * sectors_per_block;
    uint8_t *p = (uint8_t *)buf;
    for (uint32_t i = 0; i < sectors_per_block; i++) {
        ata_read(g_drive, lba + i, 1, p);
        p += 512;
    }
}

static void write_block(uint32_t blk, const void *buf) {
    uint32_t sectors_per_block = block_size / 512;
    uint32_t lba = lba_base + blk * sectors_per_block;
    const uint8_t *p = (const uint8_t *)buf;
    for (uint32_t i = 0; i < sectors_per_block; i++) {
        ata_write(g_drive, lba + i, 1, p);
        p += 512;
    }
}

static void read_inode(uint32_t ino, ext2_inode_t *out) {
    uint32_t inodes_per_block = block_size / sb.s_inode_size;
    uint32_t idx    = (ino - 1) % sb.s_inodes_per_group;
    uint32_t blk    = bgd.bg_inode_table + idx / inodes_per_block;
    uint32_t offset = (idx % inodes_per_block) * sb.s_inode_size;
    uint8_t  buf[1024];
    read_block(blk, buf);
    kmemcpy(out, buf + offset, sizeof(ext2_inode_t));
}

static void write_inode(uint32_t ino, const ext2_inode_t *in) {
    uint32_t inodes_per_block = block_size / sb.s_inode_size;
    uint32_t idx    = (ino - 1) % sb.s_inodes_per_group;
    uint32_t blk    = bgd.bg_inode_table + idx / inodes_per_block;
    uint32_t offset = (idx % inodes_per_block) * sb.s_inode_size;
    uint8_t  buf[1024];
    read_block(blk, buf);
    kmemcpy(buf + offset, in, sizeof(ext2_inode_t));
    write_block(blk, buf);
}

static uint32_t get_block(ext2_inode_t *ino, uint32_t n) {
    if (n < 12) return ino->i_block[n];
    uint32_t indirect[256];
    if (n < 12 + block_size/4) {
        read_block(ino->i_block[12], indirect);
        return indirect[n - 12];
    }
    return 0;
}

static uint32_t block_bit_alloc(void) {
    uint8_t bmp[1024];
    read_block(bgd.bg_block_bitmap, bmp);
    uint32_t usable = sb.s_blocks_count - sb.s_first_data_block;
    if (usable > EXT2_BITMAP_BITS) usable = EXT2_BITMAP_BITS;
    for (uint32_t i = 0; i < usable; i++) {
        uint32_t byte = i/8, bit = i%8;
        if (!(bmp[byte] & (1u<<bit))) {
            bmp[byte] |= (uint8_t)(1u<<bit);
            write_block(bgd.bg_block_bitmap, bmp);
            sb.s_free_blocks_count--;
            bgd.bg_free_blocks_count--;
            return sb.s_first_data_block + i;
        }
    }
    return 0;
}

static void block_bit_free(uint32_t blk) {
    if (!blk) return;
    uint8_t bmp[1024];
    read_block(bgd.bg_block_bitmap, bmp);
    uint32_t i = blk - sb.s_first_data_block;
    if (blk < sb.s_first_data_block || i >= EXT2_BITMAP_BITS) return;
    uint32_t byte = i/8, bit = i%8;
    if (bmp[byte] & (1u<<bit)) {
        bmp[byte] &= (uint8_t)~(1u<<bit);
        write_block(bgd.bg_block_bitmap, bmp);
        sb.s_free_blocks_count++;
        bgd.bg_free_blocks_count++;
    }
}

static uint32_t inode_bit_alloc(void) {
    uint8_t bmp[1024];
    read_block(bgd.bg_inode_bitmap, bmp);
    uint32_t limit = sb.s_inodes_count < EXT2_BITMAP_BITS ? sb.s_inodes_count : EXT2_BITMAP_BITS;
    for (uint32_t i = 0; i < limit; i++) {
        uint32_t byte = i/8, bit = i%8;
        if (!(bmp[byte] & (1u<<bit))) {
            bmp[byte] |= (uint8_t)(1u<<bit);
            write_block(bgd.bg_inode_bitmap, bmp);
            sb.s_free_inodes_count--;
            bgd.bg_free_inodes_count--;
            return i + 1;
        }
    }
    return 0;
}

static void inode_bit_free(uint32_t ino) {
    if (!ino) return;
    uint8_t bmp[1024];
    read_block(bgd.bg_inode_bitmap, bmp);
    uint32_t i = ino - 1;
    if (i >= EXT2_BITMAP_BITS) return;
    uint32_t byte = i/8, bit = i%8;
    if (bmp[byte] & (1u<<bit)) {
        bmp[byte] &= (uint8_t)~(1u<<bit);
        write_block(bgd.bg_inode_bitmap, bmp);
        sb.s_free_inodes_count++;
        bgd.bg_free_inodes_count++;
    }
}

static uint32_t get_or_alloc_block(ext2_inode_t *ino, uint32_t n, int *dirty) {
    if (n < 12) {
        if (!ino->i_block[n]) {
            uint32_t nb = block_bit_alloc();
            if (!nb) return 0;
            ino->i_block[n] = nb;
            *dirty = 1;
        }
        return ino->i_block[n];
    }
    uint32_t idx = n - 12;
    if (idx >= block_size/4) return 0;
    uint8_t ind[1024];
    if (!ino->i_block[12]) {
        uint32_t nb = block_bit_alloc();
        if (!nb) return 0;
        ino->i_block[12] = nb;
        *dirty = 1;
        kmemset(ind, 0, 1024);
        write_block(nb, ind);
    } else {
        read_block(ino->i_block[12], ind);
    }
    uint32_t *iptr = (uint32_t *)ind;
    if (!iptr[idx]) {
        uint32_t nb = block_bit_alloc();
        if (!nb) return 0;
        iptr[idx] = nb;
        write_block(ino->i_block[12], ind);
    }
    return iptr[idx];
}

static void flush_sb_bgd(void) {
    uint8_t buf[1024];
    kmemcpy(buf, &sb, sizeof(sb));
    write_block(1, buf);
    kmemset(buf, 0, 1024);
    kmemcpy(buf, &bgd, sizeof(bgd));
    write_block(2, buf);
}

static uint32_t lookup_dir(uint32_t dir_ino, const char *name) {
    ext2_inode_t inode;
    read_inode(dir_ino, &inode);
    uint32_t size = inode.i_size;
    uint32_t read = 0;
    uint8_t  blk[1024];
    uint32_t nlen = (uint32_t)kstrlen(name);

    for (uint32_t b = 0; read < size; b++) {
        uint32_t bno = get_block(&inode, b);
        if (!bno) break;
        read_block(bno, blk);
        uint32_t off = 0;
        uint32_t bsz = size - read > block_size ? block_size : size - read;
        while (off < bsz) {
            ext2_dirent_t *d = (ext2_dirent_t *)(blk + off);
            if (!d->rec_len) break;
            if (d->inode && d->name_len == nlen &&
                kstrncmp(d->name, name, nlen) == 0)
                return d->inode;
            off += d->rec_len;
        }
        read += block_size;
    }
    return 0;
}

static uint32_t path_to_ino(const char *path) {
    if (!*path || (path[0]=='/'&&!path[1])) return EXT2_ROOT_INO;
    const char *p = path;
    if (*p=='/') p++;
    uint32_t ino = EXT2_ROOT_INO;
    char part[256];
    while (*p) {
        const char *slash = p;
        while (*slash && *slash!='/') slash++;
        uint32_t len = (uint32_t)(slash - p);
        if (len > 255) return 0;
        kmemcpy(part, p, len); part[len]=0;
        ino = lookup_dir(ino, part);
        if (!ino) return 0;
        p = *slash ? slash+1 : slash;
    }
    return ino;
}

static void free_inode_blocks(ext2_inode_t *inode) {
    for (int i = 0; i < 12; i++) {
        if (inode->i_block[i]) {
            block_bit_free(inode->i_block[i]);
            inode->i_block[i] = 0;
        }
    }
    if (inode->i_block[12]) {
        uint8_t ind[1024];
        read_block(inode->i_block[12], ind);
        uint32_t *p = (uint32_t *)ind;
        for (uint32_t i = 0; i < block_size/4; i++)
            if (p[i]) block_bit_free(p[i]);
        block_bit_free(inode->i_block[12]);
        inode->i_block[12] = 0;
    }
}

static int add_dirent(uint32_t dir_ino, const char *name, uint32_t ino, uint8_t ftype) {
    ext2_inode_t dirinode;
    read_inode(dir_ino, &dirinode);
    uint32_t nlen = (uint32_t)kstrlen(name);
    uint32_t needed = (8 + nlen + 3) & ~3u;

    uint32_t nblocks = dirinode.i_size / block_size;
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t bno = get_block(&dirinode, b);
        if (!bno) continue;
        uint8_t dbuf[1024];
        read_block(bno, dbuf);
        uint32_t off = 0;
        while (off < block_size) {
            ext2_dirent_t *d = (ext2_dirent_t *)(dbuf + off);
            if (!d->rec_len) break;
            uint32_t used = d->inode ? ((8u + d->name_len + 3) & ~3u) : 0;
            uint32_t slack = d->rec_len - used;
            if (slack >= needed) {
                uint16_t old_rec = d->rec_len;
                if (d->inode) {
                    d->rec_len = (uint16_t)used;
                    ext2_dirent_t *nd = (ext2_dirent_t *)(dbuf + off + used);
                    nd->inode = ino;
                    nd->rec_len = (uint16_t)(old_rec - used);
                    nd->name_len = (uint8_t)nlen;
                    nd->file_type = ftype;
                    kmemcpy(nd->name, name, nlen);
                } else {
                    d->inode = ino;
                    d->name_len = (uint8_t)nlen;
                    d->file_type = ftype;
                    kmemcpy(d->name, name, nlen);
                }
                write_block(bno, dbuf);
                return 0;
            }
            off += d->rec_len;
        }
    }

    int dirty = 0;
    uint32_t bno = get_or_alloc_block(&dirinode, nblocks, &dirty);
    if (!bno) return -1;
    uint8_t dbuf[1024];
    kmemset(dbuf, 0, 1024);
    ext2_dirent_t *d = (ext2_dirent_t *)dbuf;
    d->inode = ino;
    d->rec_len = (uint16_t)block_size;
    d->name_len = (uint8_t)nlen;
    d->file_type = ftype;
    kmemcpy(d->name, name, nlen);
    write_block(bno, dbuf);

    dirinode.i_size += block_size;
    dirinode.i_blocks += block_size/512;
    write_inode(dir_ino, &dirinode);
    return 0;
}

static int remove_dirent(uint32_t dir_ino, const char *name) {
    ext2_inode_t dirinode;
    read_inode(dir_ino, &dirinode);
    uint32_t nlen = (uint32_t)kstrlen(name);
    uint32_t nblocks = dirinode.i_size / block_size;
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t bno = get_block(&dirinode, b);
        if (!bno) continue;
        uint8_t dbuf[1024];
        read_block(bno, dbuf);
        uint32_t off = 0;
        int changed = 0;
        while (off < block_size) {
            ext2_dirent_t *d = (ext2_dirent_t *)(dbuf + off);
            if (!d->rec_len) break;
            if (d->inode && d->name_len == nlen && kstrncmp(d->name, name, nlen) == 0) {
                d->inode = 0;
                changed = 1;
                break;
            }
            off += d->rec_len;
        }
        if (changed) { write_block(bno, dbuf); return 0; }
    }
    return -1;
}

int ext2_init(int drive, uint32_t lba_start) {
    g_drive  = drive;
    lba_base = lba_start;
    uint8_t tmp[1024];
    ata_read(g_drive, lba_start+2, 1, tmp);
    ata_read(g_drive, lba_start+3, 1, tmp+512);
    kmemcpy(&sb, tmp, sizeof(ext2_super_t));
    mounted = 0;
    if (sb.s_magic != EXT2_MAGIC || sb.s_log_block_size != 0) return -1;
    if (sb.s_inodes_per_group == 0 || sb.s_blocks_count > sb.s_blocks_per_group) return -1;
    if (sb.s_rev_level == 0) sb.s_inode_size = 128;
    if (sb.s_inode_size < 128 || sb.s_inode_size > 1024 || (1024 % sb.s_inode_size)) return -1;
    block_size = 1024;
    uint8_t bgd_buf[512];
    uint32_t bgd_lba = lba_start + (block_size==1024?4:block_size/512);
    ata_read(g_drive, bgd_lba, 1, bgd_buf);
    kmemcpy(&bgd, bgd_buf, sizeof(ext2_bgd_t));
    mounted = 1;
    return 0;
}

int ext2_mounted(void) { return mounted; }

int ext2_read_file(const char *path, void *buf, uint32_t sz) {
    if (!mounted) return -1;
    uint32_t ino = path_to_ino(path);
    if (!ino) return -1;
    ext2_inode_t inode;
    read_inode(ino, &inode);
    uint32_t total = inode.i_size < sz ? inode.i_size : sz;
    uint8_t blk[1024]; uint32_t done=0;
    for (uint32_t b=0; done<total; b++) {
        uint32_t bno=get_block(&inode,b); if(!bno) break;
        read_block(bno, blk);
        uint32_t chunk=total-done>block_size?block_size:total-done;
        kmemcpy((uint8_t*)buf+done, blk, chunk);
        done+=chunk;
    }
    return (int)done;
}

int ext2_write_file(const char *path, const void *buf, uint32_t size) {
    if (!mounted) return -1;
    const char *name = path;
    if (*name == '/') name++;
    if (!*name || kstrchr(name, '/')) return -1;

    uint32_t ino = lookup_dir(EXT2_ROOT_INO, name);
    ext2_inode_t inode;
    int is_new = 0;

    if (ino) {
        read_inode(ino, &inode);
        free_inode_blocks(&inode);
    } else {
        ino = inode_bit_alloc();
        if (!ino) { flush_sb_bgd(); return -1; }
        kmemset(&inode, 0, sizeof(inode));
        is_new = 1;
    }

    inode.i_mode = 0x81A4;
    inode.i_links_count = 1;

    uint32_t written = 0, blocks_used = 0;
    const uint8_t *src = (const uint8_t *)buf;
    while (written < size) {
        int dirty = 0;
        uint32_t bno = get_or_alloc_block(&inode, blocks_used, &dirty);
        if (!bno) break;
        uint8_t chunk[1024];
        kmemset(chunk, 0, 1024);
        uint32_t take = size - written > block_size ? block_size : size - written;
        kmemcpy(chunk, src + written, take);
        write_block(bno, chunk);
        written += take;
        blocks_used++;
    }

    inode.i_size = written;
    inode.i_blocks = blocks_used * (block_size/512);
    if (inode.i_block[12]) inode.i_blocks += block_size/512;
    write_inode(ino, &inode);

    if (is_new) add_dirent(EXT2_ROOT_INO, name, ino, EXT2_FT_REG);

    flush_sb_bgd();
    return (int)written;
}

int ext2_delete_file(const char *path) {
    if (!mounted) return -1;
    const char *name = path;
    if (*name == '/') name++;
    if (!*name || kstrchr(name, '/')) return -1;

    uint32_t ino = lookup_dir(EXT2_ROOT_INO, name);
    if (!ino) return -1;

    ext2_inode_t inode;
    read_inode(ino, &inode);
    free_inode_blocks(&inode);
    inode.i_links_count = 0;
    inode.i_dtime = 1;
    write_inode(ino, &inode);
    inode_bit_free(ino);
    remove_dirent(EXT2_ROOT_INO, name);

    flush_sb_bgd();
    return 0;
}

int ext2_format(int drive, uint32_t lba_start) {
    g_drive    = drive;
    lba_base   = lba_start;
    block_size = 1024;

    uint32_t total_blocks = EXT2_FMT_BLOCKS;
    uint32_t inode_count  = EXT2_FMT_INODES;
    uint32_t inode_table_blocks = (inode_count * 128 + block_size - 1) / block_size;
    uint32_t bb_blk = 3, ib_blk = 4, it_blk = 5;
    uint32_t root_blk = it_blk + inode_table_blocks;

    uint8_t zero[1024]; kmemset(zero, 0, 1024);
    write_block(0, zero);

    kmemset(&sb, 0, sizeof(sb));
    sb.s_inodes_count       = inode_count;
    sb.s_blocks_count       = total_blocks;
    sb.s_r_blocks_count     = 0;
    sb.s_first_data_block   = 1;
    sb.s_log_block_size     = 0;
    sb.s_log_frag_size      = 0;
    sb.s_blocks_per_group   = total_blocks;
    sb.s_frags_per_group    = total_blocks;
    sb.s_inodes_per_group   = inode_count;
    sb.s_magic              = EXT2_MAGIC;
    sb.s_state              = 1;
    sb.s_errors             = 1;
    sb.s_rev_level          = 1;
    sb.s_first_ino          = 11;
    sb.s_inode_size         = 128;
    sb.s_free_blocks_count  = (total_blocks - 1) - root_blk;
    sb.s_free_inodes_count  = inode_count - 10;

    kmemset(&bgd, 0, sizeof(bgd));
    bgd.bg_block_bitmap      = bb_blk;
    bgd.bg_inode_bitmap      = ib_blk;
    bgd.bg_inode_table       = it_blk;
    bgd.bg_free_blocks_count = (uint16_t)sb.s_free_blocks_count;
    bgd.bg_free_inodes_count = (uint16_t)sb.s_free_inodes_count;
    bgd.bg_used_dirs_count   = 1;

    uint8_t bbmp[1024]; kmemset(bbmp, 0, 1024);
    for (uint32_t blk = 1; blk <= root_blk; blk++) {
        uint32_t i = blk - 1, byte = i/8, bit = i%8;
        bbmp[byte] |= (uint8_t)(1u<<bit);
    }
    write_block(bb_blk, bbmp);

    uint8_t ibmp[1024]; kmemset(ibmp, 0, 1024);
    for (int i = 0; i < 10; i++) ibmp[i/8] |= (uint8_t)(1u<<(i%8));
    write_block(ib_blk, ibmp);

    uint8_t itbuf[1024]; kmemset(itbuf, 0, 1024);
    for (uint32_t i = 0; i < inode_table_blocks; i++) write_block(it_blk + i, itbuf);

    ext2_inode_t root; kmemset(&root, 0, sizeof(root));
    root.i_mode        = 0x41ED;
    root.i_size        = block_size;
    root.i_links_count = 2;
    root.i_blocks      = block_size/512;
    root.i_block[0]    = root_blk;

    kmemset(itbuf, 0, 1024);
    kmemcpy(itbuf + 128, &root, sizeof(root));
    write_block(it_blk, itbuf);

    uint8_t dblk[1024]; kmemset(dblk, 0, 1024);
    ext2_dirent_t *d1 = (ext2_dirent_t *)dblk;
    d1->inode = EXT2_ROOT_INO; d1->rec_len = 12; d1->name_len = 1; d1->file_type = EXT2_FT_DIR;
    d1->name[0] = '.';
    ext2_dirent_t *d2 = (ext2_dirent_t *)(dblk + 12);
    d2->inode = EXT2_ROOT_INO; d2->rec_len = (uint16_t)(block_size - 12);
    d2->name_len = 2; d2->file_type = EXT2_FT_DIR;
    d2->name[0] = '.'; d2->name[1] = '.';
    write_block(root_blk, dblk);

    flush_sb_bgd();
    mounted = 1;
    return 0;
}

int ext2_list_dir(const char *path, char *buf, uint32_t sz) {
    if (!mounted) return -1;
    uint32_t ino = path_to_ino(path);
    if (!ino) return -1;
    ext2_inode_t inode;
    read_inode(ino, &inode);
    uint8_t blk[1024]; uint32_t pos=0, read=0;
    for (uint32_t b=0; read<inode.i_size; b++) {
        uint32_t bno=get_block(&inode,b); if(!bno) break;
        read_block(bno, blk);
        uint32_t off=0;
        while(off<block_size) {
            ext2_dirent_t *d=(ext2_dirent_t*)(blk+off);
            if(!d->rec_len) break;
            if(d->inode && d->name_len &&
               !(d->name_len==1&&d->name[0]=='.') &&
               !(d->name_len==2&&d->name[0]=='.'&&d->name[1]=='.')) {
                if(pos+d->name_len+2<sz) {
                    kmemcpy(buf+pos,d->name,d->name_len);
                    pos+=d->name_len;
                    buf[pos++]='\n';
                }
            }
            off+=d->rec_len;
        }
        read+=block_size;
    }
    buf[pos]=0;
    return (int)pos;
}

int ext2_stat(const char *path, uint32_t *size_out, int *is_dir) {
    if (!mounted) return -1;
    uint32_t ino = path_to_ino(path);
    if (!ino) return -1;
    ext2_inode_t inode; read_inode(ino, &inode);
    if (size_out) *size_out = inode.i_size;
    if (is_dir) *is_dir = (inode.i_mode & 0xF000) == 0x4000;
    return 0;
}

static int ext2_read_at(uint32_t ino, uint32_t off, void *buf, uint32_t len) {
    ext2_inode_t inode; read_inode(ino, &inode);
    if (off >= inode.i_size) return 0;
    if (len > inode.i_size - off) len = inode.i_size - off;
    uint8_t blk[1024]; uint32_t done = 0;
    while (done < len) {
        uint32_t pos = off + done;
        uint32_t bno = get_block(&inode, pos / 1024);
        uint32_t boff = pos % 1024;
        uint32_t chunk = 1024 - boff;
        if (chunk > len - done) chunk = len - done;
        if (bno) { read_block(bno, blk); kmemcpy((uint8_t *)buf + done, blk + boff, chunk); }
        else kmemset((uint8_t *)buf + done, 0, chunk);
        done += chunk;
    }
    return (int)done;
}

int ext2_space(uint32_t *total_kb, uint32_t *free_kb) {
    if (!mounted) return -1;
    if (total_kb) *total_kb = sb.s_blocks_count * block_size / 1024;
    if (free_kb)  *free_kb  = sb.s_free_blocks_count * block_size / 1024;
    return 0;
}

void ext2_info(void) {
    if (!mounted) { vga_puts("  No EXT2 volume mounted.\n"); return; }
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    vga_puts("\n  === EXT2 Volume ===\n\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_puts("  Block size:     "); vga_put_dec(block_size); vga_puts(" bytes\n");
    vga_puts("  Total blocks:   "); vga_put_dec(sb.s_blocks_count);
    vga_puts("  Free blocks:  "); vga_put_dec(sb.s_free_blocks_count); vga_putchar('\n');
    vga_puts("  Total inodes:   "); vga_put_dec(sb.s_inodes_count);
    vga_puts("  Free inodes:  "); vga_put_dec(sb.s_free_inodes_count); vga_putchar('\n');
    vga_puts("  Free space:     "); vga_put_dec(sb.s_free_blocks_count * block_size / 1024);
    vga_puts(" KB\n\n");
}

#define E2_MAX_OPEN 8
static struct {
    int      used;
    uint32_t ino;
    uint32_t pos;
    int      writable;
    char     name[64];
    uint8_t *wbuf;
    uint32_t wsize;
} e2_open_tab[E2_MAX_OPEN];

static int e2_open(const char *path, int flags) {
    if (!mounted) return -1;
    int slot = -1;
    for (int i = 0; i < E2_MAX_OPEN; i++) if (!e2_open_tab[i].used) { slot = i; break; }
    if (slot < 0) return -1;
    uint32_t ino = path_to_ino(path);
    int writable = (flags & (O_WRONLY | O_RDWR | O_CREAT)) != 0;
    if (!ino && !(flags & O_CREAT)) return -1;

    e2_open_tab[slot].ino = ino;
    e2_open_tab[slot].pos = 0;
    e2_open_tab[slot].writable = writable;
    e2_open_tab[slot].wbuf = 0;
    e2_open_tab[slot].wsize = 0;
    kstrncpy_safe(e2_open_tab[slot].name, path, sizeof(e2_open_tab[slot].name));

    if (writable) {
        uint8_t *b = (uint8_t *)kmalloc(EXT2_WBUF_MAX);
        if (!b) return -1;
        uint32_t n = 0;
        if (ino && !(flags & O_TRUNC)) {
            int r = ext2_read_at(ino, 0, b, EXT2_WBUF_MAX);
            n = r > 0 ? (uint32_t)r : 0;
        }
        e2_open_tab[slot].wbuf = b;
        e2_open_tab[slot].wsize = n;
        e2_open_tab[slot].pos = (flags & O_APPEND) ? n : 0;
    }
    e2_open_tab[slot].used = 1;
    return slot;
}

static int e2_close(int d) {
    if (d < 0 || d >= E2_MAX_OPEN || !e2_open_tab[d].used) return -1;
    int r = 0;
    if (e2_open_tab[d].wbuf) {
        if (ext2_write_file(e2_open_tab[d].name, e2_open_tab[d].wbuf, e2_open_tab[d].wsize) < 0) r = -1;
        kfree(e2_open_tab[d].wbuf);
    }
    e2_open_tab[d].used = 0;
    return r;
}

static int e2_read(int d, void *buf, uint32_t len) {
    if (d < 0 || d >= E2_MAX_OPEN || !e2_open_tab[d].used) return -1;
    int n;
    if (e2_open_tab[d].wbuf) {
        uint32_t avail = e2_open_tab[d].wsize > e2_open_tab[d].pos ? e2_open_tab[d].wsize - e2_open_tab[d].pos : 0;
        if (len > avail) len = avail;
        kmemcpy(buf, e2_open_tab[d].wbuf + e2_open_tab[d].pos, len);
        n = (int)len;
    } else {
        n = ext2_read_at(e2_open_tab[d].ino, e2_open_tab[d].pos, buf, len);
    }
    if (n > 0) e2_open_tab[d].pos += (uint32_t)n;
    return n;
}

static int e2_write(int d, const void *buf, uint32_t len) {
    if (d < 0 || d >= E2_MAX_OPEN || !e2_open_tab[d].used || !e2_open_tab[d].wbuf) return -1;
    uint32_t pos = e2_open_tab[d].pos;
    if (pos >= EXT2_WBUF_MAX) return 0;
    if (len > EXT2_WBUF_MAX - pos) len = EXT2_WBUF_MAX - pos;
    kmemcpy(e2_open_tab[d].wbuf + pos, buf, len);
    e2_open_tab[d].pos = pos + len;
    if (e2_open_tab[d].pos > e2_open_tab[d].wsize) e2_open_tab[d].wsize = e2_open_tab[d].pos;
    return (int)len;
}

static int e2_stat(const char *path, vfs_stat_t *st) {
    uint32_t size = 0; int is_dir = 0;
    if (ext2_stat(path, &size, &is_dir) < 0) return -1;
    st->size = size;
    st->type = is_dir ? VFS_DIR : VFS_FILE;
    kstrncpy_safe(st->name, path, sizeof(st->name));
    return 0;
}

static int e2_readdir(const char *path, char *buf, uint32_t sz) {
    return ext2_list_dir(path, buf, sz);
}

static int e2_unlink(const char *path) {
    return ext2_delete_file(path);
}

#define E2_LOCKED(call) do { uint32_t f_ = irq_save(); int r_ = (call); irq_restore(f_); return r_; } while (0)
static int e2_open_l(const char *p, int fl)               { E2_LOCKED(e2_open(p, fl)); }
static int e2_close_l(int d)                              { E2_LOCKED(e2_close(d)); }
static int e2_read_l(int d, void *b, uint32_t n)          { E2_LOCKED(e2_read(d, b, n)); }
static int e2_write_l(int d, const void *b, uint32_t n)   { E2_LOCKED(e2_write(d, b, n)); }
static int e2_stat_l(const char *p, vfs_stat_t *st)       { E2_LOCKED(e2_stat(p, st)); }
static int e2_readdir_l(const char *p, char *b, uint32_t n) { E2_LOCKED(e2_readdir(p, b, n)); }
static int e2_unlink_l(const char *p)                     { E2_LOCKED(e2_unlink(p)); }

vfs_ops_t ext2_vfs_ops = {
    e2_open_l, e2_close_l, e2_read_l, e2_write_l, e2_stat_l, e2_readdir_l, e2_unlink_l, 0
};
