#include "fs.h"
#include "vga.h"
#include "kstring.h"

static fs_file_t files[FS_MAX_FILES];
static int       file_count = 0;

void fs_init(void) {
    kmemset(files, 0, sizeof(files));
    file_count = 0;

    fs_create("readme.txt",
        "Welcome to KumOS!\n"
        "A minimal C-based operating system.\n"
        "Type 'help' for available commands.\n"
        "Use 'kum <cmd>' for privileged operations.\n",
        0);

    fs_create("motd.txt",
        "  A minimal x86 OS, written from scratch.\n",
        0);

    fs_create("hello.sh",
        "echo Hello from KumOS shell script!\n"
        "echo This is a demo script.\n",
        1);
}

int fs_create(const char *name, const char *data, int exec) {
    if (fs_find(name)) return -1;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used) {
            kstrcpy(files[i].name, name);
            uint32_t len = kstrlen(data);
            if (len >= FS_MAX_DATA) len = FS_MAX_DATA - 1;
            kmemcpy(files[i].data, data, len);
            files[i].data[len] = 0;
            files[i].size = len;
            files[i].used = 1;
            files[i].executable = exec;
            file_count++;
            return 0;
        }
    }
    return -1;
}

int fs_write(const char *name, const char *data) {
    fs_file_t *f = fs_find(name);
    if (!f) return fs_create(name, data, 0);
    uint32_t len = kstrlen(data);
    if (len >= FS_MAX_DATA) len = FS_MAX_DATA - 1;
    kmemcpy(f->data, data, len);
    f->data[len] = 0;
    f->size = len;
    return 0;
}

fs_file_t *fs_find(const char *name) {
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (files[i].used && kstrcmp(files[i].name, name) == 0)
            return &files[i];
    return 0;
}

int fs_delete(const char *name) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used && kstrcmp(files[i].name, name) == 0) {
            files[i].used = 0;
            file_count--;
            return 0;
        }
    }
    return -1;
}

void fs_list(void) {
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    vga_puts("  NAME                           SIZE  TYPE\n");
    vga_puts("  ----                           ----  ----\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used) continue;
        vga_puts("  ");
        vga_puts(files[i].name);

        int pad = 32 - (int)kstrlen(files[i].name);
        for (int p = 0; p < pad; p++) vga_putchar(' ');
        vga_put_dec(files[i].size);
        vga_puts("B   ");
        vga_puts(files[i].executable ? "[exe]" : "[txt]");
        vga_putchar('\n');
    }
}

int fs_file_count(void) { return file_count; }

int fs_list_names(char *buf, uint32_t sz) {
    uint32_t pos = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used) continue;
        uint32_t l = (uint32_t)kstrlen(files[i].name);
        if (pos + l + 2 >= sz) break;
        kmemcpy(buf + pos, files[i].name, l);
        pos += l;
        buf[pos++] = '\n';
    }
    if (sz) buf[pos] = 0;
    return (int)pos;
}
