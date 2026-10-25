GCC_INC := $(shell find /usr/lib/gcc -name "stddef.h" 2>/dev/null | head -1 | xargs dirname)
CC       = gcc
CFLAGS   = -m32 -g -ffreestanding -O2 -Wall -fno-stack-protector -fno-builtin \
           -mno-sse -mno-sse2 -mno-mmx -mno-80387 -mgeneral-regs-only \
           -fno-asynchronous-unwind-tables -nostdlib -nostdinc -I$(GCC_INC) -Isrc
ASM      = nasm
ASMFLAGS = -f elf32
LD       = ld
LDFLAGS  = -m elf_i386 -T linker.ld
UFLAGS   = -m32 -nostdlib -nostartfiles -static -O2 -fno-stack-protector \
           -mno-sse -mno-sse2 -mno-mmx -mno-80387 -mgeneral-regs-only \
           -fno-asynchronous-unwind-tables -fno-builtin \
           -Wl,--build-id=none -Wl,-z,norelro -Iuser
GRUB_MKR = $(shell command -v grub2-mkrescue 2>/dev/null || command -v grub-mkrescue 2>/dev/null)

KVM      = $(shell [ -r /dev/kvm ] && echo "-enable-kvm")
QEMU     = qemu-system-x86_64 $(KVM) -boot order=d -cdrom kumos.iso \
           -drive file=disk.img,format=raw,if=ide -drive file=ext2.img,format=raw,if=ide \
           -m 128M -vga std -no-reboot

KERN_OBJS = \
    boot/boot.o boot/gdt_flush.o boot/isr_stubs.o boot/sched_switch.o \
    src/kstring.o src/vga.o src/keyboard.o src/kmalloc.o \
    src/process.o src/fs.o src/gdt.o src/idt.o \
    src/timer.o src/sched.o src/paging.o \
    src/ata.o src/fat12.o src/pipe.o src/vfs.o \
    src/signal.o src/net.o src/procfs.o src/users.o \
    src/dns.o src/dmesg.o src/dhcp.o src/ext2.o src/swap.o \
    src/syscall.o src/elf.o \
    src/serial.o src/rtc.o src/mouse.o src/gui.o src/pkg.o src/pkgnet.o \
    src/sha256.o src/sha1.o src/aes.o src/bignum.o src/x509.o src/tls.o src/speaker.o src/browser.o src/kernel.o

USER_PROGS = \
    user/hello.elf user/counter.elf user/cat.elf user/sysinfo.elf \
    user/kush.elf user/ed.elf user/vi.elf user/top.elf \
    user/crond.elf user/http.elf user/grep.elf user/tar.elf \
    user/wc.elf user/sort.elf user/uniq.elf user/awk.elf user/echo.elf \
    user/fortune.elf

PKG_NAMES = hello counter cat sysinfo kush ed vi top crond http grep tar wc sort uniq awk echo
PKG_BLOBS = $(addprefix user/,$(addsuffix _blob.o,$(PKG_NAMES)))

all: kumos.bin
boot/%.o: boot/%.asm
	@$(ASM) $(ASMFLAGS) $< -o $@
src/%.o: src/%.c src/*.h
	@$(CC) $(CFLAGS) -c $< -o $@
user/%_blob.o: user/%.elf
	@cd user && objcopy -I binary -O elf32-i386 -B i386 $*.elf $*_blob.o
kumos.bin: $(KERN_OBJS) $(PKG_BLOBS) linker.ld
	@$(LD) $(LDFLAGS) -o $@ $(KERN_OBJS) $(PKG_BLOBS) 2>/tmp/kumos-ld-stderr.$$$$; ec=$$?; grep -v deprecated /tmp/kumos-ld-stderr.$$$$ 1>&2; rm -f /tmp/kumos-ld-stderr.$$$$; exit $$ec
	@echo "Kernel: $$(ls -lh $@ | awk '{print $$5}')"
user/%.elf: user/%.c user/kumos_libc.h
	@$(CC) $(UFLAGS) -Ttext=0x400000 -o $@ $<
	@strip $@
user-programs: $(USER_PROGS)
iso: kumos.bin grub.cfg
	@mkdir -p iso/boot/grub
	@strip -o iso/boot/kumos.bin kumos.bin && cp grub.cfg iso/boot/grub/grub.cfg
	@$(GRUB_MKR) --compress=xz --fonts= --locales= --themes= -o kumos.iso iso/ 2>/dev/null
	@echo "ISO: $$(ls -lh kumos.iso | awk '{print $$5}')"
ext2.img:
	@dd if=/dev/zero of=$@ bs=1M count=8 status=none
run: iso ext2.img
	@[ -r /dev/kvm ] || echo "WARNING: /dev/kvm not readable - falling back to TCG emulation, the GUI will feel slow. Add your user to the 'kvm' group."
	@$(QEMU)
run-net: iso ext2.img
	@$(QEMU) -nic user,model=rtl8139
run-serial: iso ext2.img
	@$(QEMU) -serial stdio
test: iso ext2.img
	@./smoke_test.sh
pkg-repo: user/fortune.elf
	@mkdir -p pkg-repo
	@cp user/fortune.elf pkg-repo/FORTUNE.ELF
	@echo "fortune|FORTUNE.ELF|Random one-liner (network pkg demo)" > pkg-repo/index.txt
	@echo "pkg-repo/ ready. Serve it with: python3 -m http.server 8080 --directory pkg-repo"
	@echo "Then in KumOS: pkg update && pkg install fortune"
clean:
	@rm -rf $(KERN_OBJS) $(PKG_BLOBS) kumos.bin kumos.iso iso user/*.elf
.PHONY: all iso run run-net run-serial test clean user-programs pkg-repo
