#!/bin/bash
set -e

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

log()  { echo -e "${GREEN}[ok]${NC} $1"; }
warn() { echo -e "${YELLOW}[!]${NC} $1"; }
err()  { echo -e "${RED}[x]${NC} $1"; exit 1; }

install_deps() {
    if command -v pacman &>/dev/null; then
        sudo pacman -S --needed nasm gcc make grub libisoburn mtools qemu-system-x86
    elif command -v apt-get &>/dev/null; then
        sudo apt-get install -y nasm gcc gcc-multilib make grub-pc-bin grub-common xorriso mtools qemu-system-x86
    elif command -v dnf &>/dev/null; then
        sudo dnf install -y nasm gcc make grub2-tools-extra grub2-pc-modules xorriso mtools qemu-system-x86
    else
        warn "Unknown distro. Install: nasm gcc(32-bit multilib) make grub-mkrescue xorriso qemu-system-x86_64"
        return
    fi
    log "Dependencies installed."
}

write_usb() {
    local dev="$1"
    [ -f kumos.iso ] || make iso
    if [ -z "$dev" ]; then
        lsblk -d -o NAME,SIZE,TYPE,TRAN | grep -v "loop\|rom"
        read -rp "USB device (e.g. /dev/sdb): " dev
    fi
    [ -b "$dev" ] || err "$dev is not a block device."
    echo -e "${RED}This will ERASE $dev. Type 'yes' to continue:${NC}"
    read -r confirm
    [ "$confirm" = "yes" ] || { warn "Aborted."; return; }
    sudo dd if=kumos.iso of="$dev" bs=4M status=progress oflag=sync
    log "Done. Boot from $dev."
}

case "${1:-iso}" in
    deps)  install_deps ;;
    usb)   write_usb "${2:-}" ;;
    build) make ;;
    iso)   make iso ;;
    run)   make run ;;
    net)   make run-net ;;
    test)  make test ;;
    clean) make clean ;;
    *)     echo "Usage: $0 {deps|build|iso|run|net|test|clean|usb [device]}"; exit 1 ;;
esac
