#include "vfs.h"
#include "pipe.h"
#include "fs.h"
#include "fat12.h"
#include "keyboard.h"
#include "vga.h"
#include "kstring.h"
#include "sched.h"
#include <stdint.h>

static vfs_fd_t files[VFS_MAX_FILES];
static int8_t   fdmap[SCHED_MAX_TASKS][VFS_MAX_FD];

static vfs_mount_t mounts[VFS_MAX_MOUNTS];
static int         num_mounts = 0;

static int find_mount(const char *path, const char **local) {
    int best = -1, best_len = 0;
    for (int i = 0; i < num_mounts; i++) {
        if (!mounts[i].active) continue;
        int plen = (int)kstrlen(mounts[i].prefix);
        if (kstrncmp(path, mounts[i].prefix, (uint32_t)plen) != 0) continue;
        if (path[plen] != 0 && path[plen] != '/') continue;
        if (plen > best_len) { best = i; best_len = plen; }
    }
    if (best >= 0 && local) {
        *local = path + best_len;
        if (**local == '/') (*local)++;
    }
    return best;
}

static char *cur_cwd(void) {
    return sched_current()->cwd;
}

void vfs_resolve(const char *path, char *out) {
    char tmp[VFS_MAX_PATH * 2];
    uint32_t n = 0;
    if (path[0] != '/') {
        const char *c = cur_cwd();
        if (!c[0]) c = "/disk";
        while (*c && n < sizeof(tmp) - 2) tmp[n++] = *c++;
        tmp[n++] = '/';
    }
    while (*path && n < sizeof(tmp) - 1) tmp[n++] = *path++;
    tmp[n] = 0;

    uint32_t o = 0;
    out[0] = 0;
    char *p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *seg = p;
        while (*p && *p != '/') p++;
        uint32_t len = (uint32_t)(p - seg);
        if (len == 1 && seg[0] == '.') continue;
        if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;
            out[o] = 0;
            continue;
        }
        if (o + 1 + len >= VFS_MAX_PATH) break;
        out[o++] = '/';
        for (uint32_t i = 0; i < len; i++) out[o++] = seg[i];
        out[o] = 0;
    }
    if (!o) { out[0] = '/'; out[1] = 0; }
}

#define MEM_MAX_OPEN 16
static struct { fs_file_t *f; uint32_t pos; int used; } mem_open[MEM_MAX_OPEN];

static int mem_vfs_open(const char *path, int flags) {
    fs_file_t *f = fs_find(path);
    if (!f) {
        if (!(flags & O_CREAT) || fs_create(path, "", 0) < 0) return -1;
        f = fs_find(path);
        if (!f) return -1;
    }
    if (flags & O_TRUNC) fs_write(path, "");
    for (int i = 0; i < MEM_MAX_OPEN; i++) {
        if (!mem_open[i].used) {
            mem_open[i].f    = f;
            mem_open[i].pos  = (flags & O_APPEND) ? f->size : 0;
            mem_open[i].used = 1;
            return i;
        }
    }
    return -1;
}
static int mem_vfs_close(int d) {
    if (d >= 0 && d < MEM_MAX_OPEN) mem_open[d].used = 0;
    return 0;
}
static int mem_vfs_read(int d, void *buf, uint32_t len) {
    if (d < 0 || d >= MEM_MAX_OPEN || !mem_open[d].used) return -1;
    fs_file_t *f = mem_open[d].f;
    uint32_t avail = f->size > mem_open[d].pos ? f->size - mem_open[d].pos : 0;
    if (len > avail) len = avail;
    kmemcpy(buf, f->data + mem_open[d].pos, len);
    mem_open[d].pos += len;
    return (int)len;
}
static int mem_vfs_write(int d, const void *buf, uint32_t len) {
    if (d < 0 || d >= MEM_MAX_OPEN || !mem_open[d].used) return -1;
    fs_file_t *f = mem_open[d].f;
    uint32_t pos = mem_open[d].pos;
    if (pos > f->size) pos = f->size;
    if (pos >= FS_MAX_DATA - 1) return 0;
    if (len > FS_MAX_DATA - 1 - pos) len = FS_MAX_DATA - 1 - pos;
    kmemcpy(f->data + pos, buf, len);
    pos += len;
    if (pos > f->size) f->size = pos;
    f->data[f->size] = 0;
    mem_open[d].pos = pos;
    return (int)len;
}
static int mem_vfs_stat(const char *path, vfs_stat_t *st) {
    if (!*path) { st->size = 0; st->type = VFS_DIR; kstrcpy(st->name, "mem"); return 0; }
    fs_file_t *f = fs_find(path);
    if (!f) return -1;
    st->size = f->size; st->type = VFS_FILE;
    kstrncpy_safe(st->name, f->name, VFS_NAME_MAX);
    return 0;
}
static int mem_vfs_readdir(const char *path, char *buf, uint32_t sz) {
    (void)path;
    return fs_list_names(buf, sz);
}
static int mem_vfs_unlink(const char *path) { return fs_delete(path); }

static vfs_ops_t mem_ops = {
    mem_vfs_open, mem_vfs_close, mem_vfs_read, mem_vfs_write,
    mem_vfs_stat, mem_vfs_readdir, mem_vfs_unlink, 0
};

#define DISK_BUF_SIZE  16384
#define DISK_MAX_OPEN  4
static struct {
    char     name[16];
    uint8_t  buf[DISK_BUF_SIZE];
    uint32_t size, pos;
    int      writable, used;
} disk_open[DISK_MAX_OPEN];

static void disk_name(const char *path, char *out) {
    int i = 0;
    while (path[i] && i < 15) {
        char c = path[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[i++] = c;
    }
    out[i] = 0;
}

static int disk_vfs_open(const char *path, int flags) {
    if (!fat12_mounted() || !*path || kstrchr(path, '/')) return -1;
    char upper[16]; disk_name(path, upper);
    for (int s = 0; s < DISK_MAX_OPEN; s++) {
        if (disk_open[s].used) continue;
        int n = fat12_read(upper, disk_open[s].buf, DISK_BUF_SIZE);
        if (n < 0) {
            if (!(flags & O_CREAT)) return -1;
            if (fat12_write(upper, "", 0) != 0) return -1;
            n = 0;
        }
        disk_open[s].size = (uint32_t)n;
        disk_open[s].pos  = (flags & O_APPEND) ? (uint32_t)n : 0;
        if (flags & O_TRUNC) disk_open[s].size = disk_open[s].pos = 0;
        disk_open[s].writable = (flags & (O_WRONLY|O_RDWR|O_CREAT|O_TRUNC|O_APPEND)) ? 1 : 0;
        disk_open[s].used = 1;
        kstrcpy(disk_open[s].name, upper);
        return s;
    }
    return -1;
}
static int disk_vfs_close(int d) {
    if (d < 0 || d >= DISK_MAX_OPEN || !disk_open[d].used) return -1;
    int r = 0;
    if (disk_open[d].writable)
        r = fat12_write(disk_open[d].name, disk_open[d].buf, disk_open[d].size);
    disk_open[d].used = 0;
    return r;
}
static int disk_vfs_read(int d, void *buf, uint32_t len) {
    if (d < 0 || d >= DISK_MAX_OPEN || !disk_open[d].used) return -1;
    uint32_t avail = disk_open[d].size > disk_open[d].pos ? disk_open[d].size - disk_open[d].pos : 0;
    if (len > avail) len = avail;
    kmemcpy(buf, disk_open[d].buf + disk_open[d].pos, len);
    disk_open[d].pos += len;
    return (int)len;
}
static int disk_vfs_write(int d, const void *buf, uint32_t len) {
    if (d < 0 || d >= DISK_MAX_OPEN || !disk_open[d].used) return -1;
    uint32_t pos = disk_open[d].pos;
    if (pos >= DISK_BUF_SIZE) return 0;
    if (len > DISK_BUF_SIZE - pos) len = DISK_BUF_SIZE - pos;
    kmemcpy(disk_open[d].buf + pos, buf, len);
    disk_open[d].pos = pos + len;
    if (disk_open[d].pos > disk_open[d].size) disk_open[d].size = disk_open[d].pos;
    return (int)len;
}
static int disk_vfs_stat(const char *path, vfs_stat_t *st) {
    if (!fat12_mounted()) return -1;
    if (!*path) { st->size = 0; st->type = VFS_DIR; kstrcpy(st->name, "disk"); return 0; }
    char upper[16]; disk_name(path, upper);
    static fat12_entry_t entries[64];
    int n = fat12_list(entries, 64);
    for (int j = 0; j < n; j++) {
        if (kstrcmp(entries[j].name, upper) == 0) {
            st->size = entries[j].size;
            st->type = entries[j].is_dir ? VFS_DIR : VFS_FILE;
            kstrcpy(st->name, entries[j].name);
            return 0;
        }
    }
    return -1;
}
static int disk_vfs_readdir(const char *path, char *buf, uint32_t sz) {
    if (!fat12_mounted() || *path) { buf[0] = 0; return 0; }
    static fat12_entry_t entries[64];
    int n = fat12_list(entries, 64);
    uint32_t pos = 0;
    for (int i = 0; i < n && pos + 14 < sz; i++) {
        kstrcpy(buf + pos, entries[i].name);
        pos += kstrlen(entries[i].name);
        buf[pos++] = '\n';
    }
    buf[pos] = 0;
    return (int)pos;
}
static int disk_vfs_unlink(const char *path) {
    char upper[16]; disk_name(path, upper);
    return fat12_delete(upper);
}

static vfs_ops_t disk_ops = {
    disk_vfs_open, disk_vfs_close, disk_vfs_read, disk_vfs_write,
    disk_vfs_stat, disk_vfs_readdir, disk_vfs_unlink, 0
};

static int dev_vfs_open(const char *path, int flags) {
    (void)flags;
    if (kstrcmp(path,"stdin")==0||kstrcmp(path,"tty")==0) return 0;
    if (kstrcmp(path,"stdout")==0)                         return 1;
    if (kstrcmp(path,"stderr")==0)                         return 2;
    if (kstrcmp(path,"null")==0)                           return 3;
    return -1;
}
static int dev_vfs_close(int d) { (void)d; return 0; }
static int dev_vfs_read(int d, void *buf, uint32_t len) {
    if (d == 0) {
        if (len < 2) return 0;
        int n = keyboard_getline((char *)buf, (int)len);
        if (n < 0) return 0;
        ((char *)buf)[n++] = '\n';
        return n;
    }
    if (d == 3) return 0;
    return -1;
}
static vfs_stdout_sink_t stdout_sink = 0;

void vfs_set_stdout_sink(vfs_stdout_sink_t sink) { stdout_sink = sink; }

static int dev_vfs_write(int d, const void *buf, uint32_t len) {
    if (d == 1 || d == 2) {
        const char *s = (const char *)buf;
        if (stdout_sink) { for (uint32_t i = 0; i < len; i++) stdout_sink(s[i]); }
        else              { for (uint32_t i = 0; i < len; i++) vga_putchar(s[i]); }
        return (int)len;
    }
    if (d == 3) return (int)len;
    return -1;
}
static int dev_vfs_stat(const char *path, vfs_stat_t *st) {
    st->size = 0;
    st->type = *path ? VFS_DEV : VFS_DIR;
    kstrncpy_safe(st->name, *path ? path : "dev", VFS_NAME_MAX);
    return 0;
}
static int dev_vfs_readdir(const char *path, char *buf, uint32_t sz) {
    (void)path;
    kstrncpy_safe(buf, "stdin\nstdout\nstderr\ntty\nnull\n", sz);
    return (int)kstrlen(buf);
}

static vfs_ops_t dev_ops = {
    dev_vfs_open, dev_vfs_close, dev_vfs_read, dev_vfs_write,
    dev_vfs_stat, dev_vfs_readdir, 0, 0
};

void vfs_init(void) {
    kmemset(files, 0, sizeof(files));
    kmemset(fdmap, -1, sizeof(fdmap));
    kmemset(mounts,   0, sizeof(mounts));
    kmemset(mem_open, 0, sizeof(mem_open));
    kmemset(disk_open,0, sizeof(disk_open));
    num_mounts = 0;

    pipe_init();

    vfs_mount("/mem",  &mem_ops);
    vfs_mount("/disk", &disk_ops);
    vfs_mount("/dev",  &dev_ops);

    for (int i = 0; i < 3; i++) {
        files[i].refs      = 1;
        files[i].type      = VFS_DEV;
        files[i].mount_idx = find_mount("/dev", 0);
        files[i].fd_data   = i;
        files[i].flags     = (i == 0) ? O_RDONLY : O_WRONLY;
        files[i].pipe_id   = -1;
    }
}

int vfs_mount(const char *prefix, vfs_ops_t *ops) {
    if (num_mounts >= VFS_MAX_MOUNTS) return -1;
    kstrncpy_safe(mounts[num_mounts].prefix, prefix, sizeof(mounts[num_mounts].prefix));
    mounts[num_mounts].ops    = ops;
    mounts[num_mounts].active = 1;
    num_mounts++;
    return 0;
}

static int alloc_file(void) {
    for (int i = 3; i < VFS_MAX_FILES; i++)
        if (!files[i].refs) { kmemset(&files[i], 0, sizeof(files[i])); files[i].pipe_id = -1; return i; }
    return -1;
}

static void file_unref(int idx) {
    if (idx < 0 || idx >= VFS_MAX_FILES || !files[idx].refs) return;
    if (--files[idx].refs > 0 || idx < 3) {
        if (idx < 3 && files[idx].refs == 0) files[idx].refs = 1;
        return;
    }
    vfs_fd_t *f = &files[idx];
    if (f->type == VFS_PIPE) {
        if (f->pipe_end == 0) pipe_close_read(f->pipe_id);
        else                  pipe_close_write(f->pipe_id);
    } else if (f->mount_idx >= 0 && mounts[f->mount_idx].ops->close) {
        mounts[f->mount_idx].ops->close(f->fd_data);
    }
}

static int8_t *my_fds(void) {
    return fdmap[sched_current_index()];
}

static int alloc_fd(int8_t *map) {
    for (int i = 3; i < VFS_MAX_FD; i++)
        if (map[i] < 0) return i;
    return -1;
}

static vfs_fd_t *fd_file(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FD) return 0;
    int idx = my_fds()[fd];
    return (idx >= 0) ? &files[idx] : 0;
}

void vfs_task_init(int slot) {
    if (slot < 0 || slot >= SCHED_MAX_TASKS) return;
    vfs_task_exit(slot);
    for (int i = 0; i < 3; i++) { fdmap[slot][i] = (int8_t)i; files[i].refs++; }
}

void vfs_task_exit(int slot) {
    if (slot < 0 || slot >= SCHED_MAX_TASKS) return;
    for (int i = 0; i < VFS_MAX_FD; i++) {
        if (fdmap[slot][i] >= 0) { file_unref(fdmap[slot][i]); fdmap[slot][i] = -1; }
    }
}

void vfs_fork(int parent_slot, int child_slot) {
    vfs_task_exit(child_slot);
    for (int i = 0; i < VFS_MAX_FD; i++) {
        int idx = fdmap[parent_slot][i];
        fdmap[child_slot][i] = (int8_t)idx;
        if (idx >= 0) files[idx].refs++;
    }
}

int vfs_open(const char *path, int flags) {
    char resolved[VFS_MAX_PATH];
    vfs_resolve(path, resolved);

    const char *local = 0;
    int midx = find_mount(resolved, &local);
    vfs_stat_t probe;

    if (path[0] != '/' && (midx < 0 || (!(flags & O_CREAT) && mounts[midx].ops->stat &&
                                         mounts[midx].ops->stat(local, &probe) < 0))) {
        char mem_path[VFS_MAX_PATH] = "/mem/";
        kstrncpy_safe(mem_path + 5, path, VFS_MAX_PATH - 5);
        const char *mlocal = 0;
        int m2 = find_mount(mem_path, &mlocal);
        if (m2 >= 0 && mounts[m2].ops->stat(mlocal, &probe) == 0) {
            midx = m2;
            kstrcpy(resolved, mem_path);
            find_mount(resolved, &local);
        }
    }
    if (midx < 0 || !mounts[midx].ops->open) return -1;

    int8_t *map = my_fds();
    int fd = alloc_fd(map);
    int idx = alloc_file();
    if (fd < 0 || idx < 0) return -1;

    int data = mounts[midx].ops->open(local, flags);
    if (data < 0) return -1;

    files[idx].refs      = 1;
    files[idx].flags     = flags;
    files[idx].type      = VFS_FILE;
    files[idx].mount_idx = midx;
    files[idx].fd_data   = data;
    map[fd] = (int8_t)idx;
    return fd;
}

int vfs_close(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FD) return -1;
    int8_t *map = my_fds();
    if (map[fd] < 0) return -1;
    int idx = map[fd];
    map[fd] = -1;
    file_unref(idx);
    return 0;
}

int vfs_read(int fd, void *buf, uint32_t len) {
    vfs_fd_t *f = fd_file(fd);
    if (!f) return -1;
    if (f->type == VFS_PIPE) return f->pipe_end == 0 ? pipe_read(f->pipe_id, buf, len) : -1;
    if (f->mount_idx < 0 || !mounts[f->mount_idx].ops->read) return -1;
    int n = mounts[f->mount_idx].ops->read(f->fd_data, buf, len);
    if (n > 0) f->pos += (uint32_t)n;
    return n;
}

int vfs_write(int fd, const void *buf, uint32_t len) {
    vfs_fd_t *f = fd_file(fd);
    if (!f) return -1;
    if (f->type == VFS_PIPE) return f->pipe_end == 1 ? pipe_write(f->pipe_id, buf, len) : -1;
    if (f->mount_idx < 0 || !mounts[f->mount_idx].ops->write) return -1;
    int n = mounts[f->mount_idx].ops->write(f->fd_data, buf, len);
    if (n > 0) f->pos += (uint32_t)n;
    return n;
}

int vfs_stat(const char *path, vfs_stat_t *st) {
    char resolved[VFS_MAX_PATH];
    vfs_resolve(path, resolved);
    if (resolved[1] == 0) { st->size = 0; st->type = VFS_DIR; kstrcpy(st->name, "/"); return 0; }
    const char *local = 0;
    int midx = find_mount(resolved, &local);
    if (midx < 0 || !mounts[midx].ops->stat) return -1;
    return mounts[midx].ops->stat(local, st);
}

int vfs_readdir(const char *path, char *buf, uint32_t sz) {
    if (!sz) return 0;
    char resolved[VFS_MAX_PATH];
    vfs_resolve(*path ? path : ".", resolved);
    if (resolved[1] == 0) {
        uint32_t pos = 0;
        for (int i = 0; i < num_mounts; i++) {
            if (!mounts[i].active) continue;
            const char *nm = mounts[i].prefix + 1;
            uint32_t l = (uint32_t)kstrlen(nm);
            if (pos + l + 2 >= sz) break;
            kmemcpy(buf + pos, nm, l); pos += l; buf[pos++] = '\n';
        }
        buf[pos] = 0;
        return (int)pos;
    }
    const char *local = 0;
    int midx = find_mount(resolved, &local);
    if (midx < 0 || !mounts[midx].ops->readdir) { buf[0] = 0; return 0; }
    return mounts[midx].ops->readdir(local, buf, sz);
}

int vfs_unlink(const char *path) {
    char resolved[VFS_MAX_PATH];
    vfs_resolve(path, resolved);
    const char *local = 0;
    int midx = find_mount(resolved, &local);
    if (midx < 0 || !*local || !mounts[midx].ops->unlink) return -1;
    return mounts[midx].ops->unlink(local);
}

int vfs_chmod(const char *path, const char *mode) {
    char resolved[VFS_MAX_PATH];
    vfs_resolve(path, resolved);
    if (kstrncmp(resolved, "/disk/", 6) != 0) return -1;

    int make_writable;
    if (mode[0] == '-') {
        make_writable = 0;
    } else if (mode[0] == '+') {
        make_writable = 1;
    } else {
        make_writable = 0;
        for (const char *p = mode; *p; p++)
            if (*p=='w' || *p=='2' || *p=='6' || *p=='7') { make_writable = 1; break; }
    }
    char upper[16]; disk_name(resolved + 6, upper);
    return fat12_set_readonly(upper, make_writable ? 0 : 1);
}

int vfs_getcwd(char *buf, uint32_t sz) {
    if (!sz) return -1;
    const char *c = cur_cwd();
    kstrncpy_safe(buf, c[0] ? c : "/disk", sz);
    return (int)kstrlen(buf);
}

int vfs_chdir(const char *path) {
    char resolved[VFS_MAX_PATH];
    vfs_resolve(path, resolved);
    vfs_stat_t st;
    if (vfs_stat(resolved, &st) != 0 || st.type != VFS_DIR) return -1;
    kstrncpy_safe(cur_cwd(), resolved, VFS_MAX_PATH);
    return 0;
}

int vfs_pipe(int fds[2]) {
    int8_t *map = my_fds();
    int r = alloc_fd(map);
    if (r < 0) return -1;
    map[r] = 0;
    int w = alloc_fd(map);
    map[r] = -1;
    if (w < 0) return -1;
    int ri = alloc_file();
    if (ri < 0) return -1;
    files[ri].refs = 1;
    int wi = alloc_file();
    if (wi < 0) { files[ri].refs = 0; return -1; }
    int id = pipe_create();
    if (id < 0) { files[ri].refs = 0; return -1; }

    files[ri].type = VFS_PIPE; files[ri].pipe_id = id; files[ri].pipe_end = 0;
    files[ri].mount_idx = -1; files[ri].flags = O_RDONLY;
    files[wi].refs = 1;
    files[wi].type = VFS_PIPE; files[wi].pipe_id = id; files[wi].pipe_end = 1;
    files[wi].mount_idx = -1; files[wi].flags = O_WRONLY;

    map[r] = (int8_t)ri;
    map[w] = (int8_t)wi;
    fds[0] = r; fds[1] = w;
    return 0;
}

int vfs_dup2(int oldfd, int newfd) {
    if (oldfd < 0 || oldfd >= VFS_MAX_FD || newfd < 0 || newfd >= VFS_MAX_FD) return -1;
    int8_t *map = my_fds();
    if (map[oldfd] < 0) return -1;
    if (newfd == oldfd) return newfd;
    if (map[newfd] >= 0) { file_unref(map[newfd]); map[newfd] = -1; }
    map[newfd] = map[oldfd];
    files[(int)map[newfd]].refs++;
    return newfd;
}

int vfs_isatty(int fd) {
    vfs_fd_t *f = fd_file(fd);
    return (f && f->type == VFS_DEV && f->fd_data < 3) ? 1 : 0;
}

vfs_fd_t *vfs_get_fd(int fd) {
    return fd_file(fd);
}
