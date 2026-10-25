KumOS
=====

A small x86 operating system written from scratch in C and NASM.
GRUB loads it via multiboot, it switches to a 1920x1080 desktop, and
every terminal window on that desktop is a real shell running as its own
ring-3 process.

Version 2.0. GPLv3.


What it has
-----------

Kernel
  - 32-bit protected mode, GDT/IDT, 8259 PIC, 100 Hz PIT
  - Preemptive round-robin scheduler, up to 16 tasks
  - Paging: every program gets its own address space; fork() copies the
    caller's pages, execve() builds a fresh image with real argc/argv
  - User faults (bad pointer, divide by zero, ...) kill only that program
    with the usual 128+signal exit code; kernel panics drop back to text
    mode and log to COM1
  - Signals: SIGINT/SIGTERM/SIGKILL/SIGSTOP/SIGCONT/SIGTSTP, SIG_IGN/SIG_DFL
  - 41 INT 0x80 syscalls; every user pointer is checked before use
  - VFS with per-process file descriptors, working directory and pipes:
      /disk   FAT12 on ATA drive 0 (read/write)
      /ext2   ext2 on ATA drive 1 (read/write)
      /mem    in-memory files
      /dev    stdin, stdout, stderr, tty, null
      /proc   ps, meminfo, uptime, version, net, date, dmesg
  - PS/2 keyboard and mouse, RTC, COM1 serial, PC speaker
  - RTL8139 network card: ARP, IPv4, UDP, TCP, ICMP, DHCP, DNS
  - TLS 1.2 client written from scratch (RSA key exchange, AES-128-CBC,
    SHA-1/SHA-256). It encrypts but does not validate certificates.
  - Bochs/QEMU VBE framebuffer at 1920x1080x32

Desktop (all built into the kernel)
  Terminal x3, Files, System Monitor, Calculator, Notepad, KumWrite,
  Packages, Web (text-mode browser, Ecosia home page), Paint, Snake,
  2048, Piano, Minesweeper, Settings (wallpaper theme, saved to disk).
  Right-click the desktop for New Terminal, Cascade, Tile, Restart and
  Shut Down. Alt+Tab switches windows, double-click a title bar to
  maximise, drag a window to a screen edge to snap it.

Userspace
  kush      the shell: pipes, redirection (> >> <), background jobs (&),
            jobs/fg/bg/wait, Ctrl+C, Ctrl+Z, variables, aliases, history,
            scripts with if/elif/else, for, while
  programs  hello counter cat sysinfo ed vi top crond http grep tar wc
            sort uniq awk echo, plus fortune from the network repo
  libc      user/kumos_libc.h, a single header (printf, malloc, string
            functions, qsort, syscall wrappers)

All built-in programs are linked into the kernel image, so they work on
any disk. Programs installed with `pkg` or copied to /disk run too.


Build
-----

Needs a Linux host with a 32-bit capable gcc, nasm, binutils, GRUB's
grub-mkrescue with xorriso, and QEMU.

  ./build.sh deps        install the tools (pacman, apt or dnf)
  make iso               build kumos.iso
  make run               boot it in QEMU
  make run-net           same, with an RTL8139 network card
  make run-serial        same, with COM1 on your terminal
  make test              headless boot test, needs nothing but QEMU
  ./build.sh usb /dev/X  write the ISO to a USB stick

`make test` boots copies of disk.img and ext2.img, so the tracked images
are never modified.


Using it
--------

The desktop opens with one terminal. Type `help` for the shell's
builtins. Files saved from the apps live on /disk (disk.img); ext2.img is
mounted at /ext2.

  ls /                  mount points
  cd /ext2; cat README.TXT
  sysinfo > info.txt    output into a file
  cat /proc/ps | wc     pipes
  counter &             background job, then `jobs`
  pkg update            fetch the network repo index (see below)

The Exit icon leaves the desktop for the kernel's text console, where
`gui` brings it back.

Network package repo: `make pkg-repo`, then serve it with
`python3 -m http.server 8080 --directory pkg-repo`. Inside KumOS run
`pkg update` and `pkg install fortune` from the text console, or use the
Packages window.


Layout
------

  boot/        multiboot entry, GDT/IDT/context-switch stubs (NASM)
  src/         kernel
  user/        userspace programs and libc
  disk.img     1.44 MB FAT12 disk (drive 0)
  ext2.img     8 MB ext2 disk (drive 1)
  pkg-repo/    demo network package repository
  linker.ld    kernel link script (fails the build if the kernel reaches 4 MB)

Memory map
  0x00100000  kernel image, must end below 0x00400000
  0x00400000  user programs (private per process, 4 MB)
  0x00800000  kernel heap (2 MB)
  0x00A00000  physical page allocator starts here
  0x10000000  kernel vmalloc window (16 MB, GUI back buffers)
  0x3FFFC000  user stack (private per process, 16 KB)
  Physical RAM is identity mapped up to 256 MB. 128 MB is the default.


Notes
-----

- Serial output: 115200 8N1 on COM1.
- No comments in source code (intentional).
- Default outbound services are European: Ecosia for the browser.
