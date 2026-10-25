#ifndef VFS_H
#define VFS_H

#include <stdint.h>

#define VFS_MAX_FD      32
#define VFS_MAX_FILES   64
#define VFS_MAX_PATH   128
#define VFS_NAME_MAX    64

#define VFS_FILE        1
#define VFS_DIR         2
#define VFS_DEV         3
#define VFS_PIPE        4

#define O_RDONLY        0x00
#define O_WRONLY        0x01
#define O_RDWR          0x02
#define O_CREAT         0x04
#define O_TRUNC         0x08
#define O_APPEND        0x10

typedef struct {
    uint32_t size;
    uint8_t  type;
    char     name[VFS_NAME_MAX];
} vfs_stat_t;

typedef struct vfs_ops {
    int  (*open) (const char *path, int flags);
    int  (*close)(int fd_data);
    int  (*read) (int fd_data, void *buf, uint32_t len);
    int  (*write)(int fd_data, const void *buf, uint32_t len);
    int  (*stat) (const char *path, vfs_stat_t *st);
    int  (*readdir)(const char *path, char *buf, uint32_t sz);
    int  (*unlink)(const char *path);
    int  (*mkdir)(const char *path);
} vfs_ops_t;

#define VFS_MAX_MOUNTS  8
typedef struct {
    char       prefix[32];
    vfs_ops_t *ops;
    int        active;
} vfs_mount_t;

typedef struct {
    int      refs;
    int      flags;
    uint32_t pos;
    int      type;
    int      mount_idx;
    int      fd_data;
    int      pipe_id;
    int      pipe_end;
} vfs_fd_t;

void vfs_init(void);
int  vfs_mount(const char *prefix, vfs_ops_t *ops);

void vfs_task_init(int slot);
void vfs_task_exit(int slot);
void vfs_fork(int parent_slot, int child_slot);

int  vfs_open  (const char *path, int flags);
int  vfs_close (int fd);
int  vfs_read  (int fd, void *buf, uint32_t len);
int  vfs_write (int fd, const void *buf, uint32_t len);
int  vfs_stat  (const char *path, vfs_stat_t *st);
int  vfs_readdir(const char *path, char *buf, uint32_t sz);
int  vfs_unlink(const char *path);
int  vfs_chmod (const char *path, const char *mode);
int  vfs_getcwd(char *buf, uint32_t sz);
int  vfs_chdir (const char *path);
void vfs_resolve(const char *path, char *out);

int  vfs_pipe  (int fds[2]);
int  vfs_dup2  (int oldfd, int newfd);
int  vfs_isatty(int fd);

#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

vfs_fd_t *vfs_get_fd(int fd);

#endif
