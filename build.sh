

#!/usr/bin/env bash
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="$ROOT/src"
BIN="$ROOT/Binaries"
DL="$ROOT/Downloads"
OBJ="$ROOT/.build"
ISO_DIR="$OBJ/iso"
IMG="$ISO_DIR/efiboot.img"
IMG_MB=40

INC=""
for d in /usr/include/efi /usr/local/include/efi; do
    if [ -d "$d" ]; then INC="$d"; break; fi
done

LIBDIR=""
for d in /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu /usr/lib64/gnuefi /usr/lib/gnuefi /usr/local/lib; do
    if [ -f "$d/crt0-efi-x86_64.o" ]; then LIBDIR="$d"; break; fi
done

LDS=""
for d in "$LIBDIR" /usr/lib /usr/lib64 /usr/lib64/gnuefi /usr/lib/gnuefi; do
    if [ -n "$d" ] && [ -f "$d/elf_x86_64_efi.lds" ]; then LDS="$d/elf_x86_64_efi.lds"; break; fi
done

if [ -z "$INC" ] || [ -z "$LIBDIR" ] || [ -z "$LDS" ]; then
    echo "gnu-efi not found. Install it (Debian/Ubuntu: gnu-efi, Fedora: gnu-efi-devel, Arch: gnu-efi)."
    exit 1
fi

mkdir -p "$BIN" "$DL" "$OBJ"

CFLAGS="-I$INC -I$INC/x86_64 -I$INC/protocol -fpic -ffreestanding -fno-stack-protector -fno-stack-check -fshort-wchar -mno-red-zone -maccumulate-outgoing-args -fno-builtin -O2 -Wall -Wextra -Wno-unused-parameter"
LDFLAGS="-nostdlib -znocombreloc -T $LDS -shared -Bsymbolic -L$LIBDIR $LIBDIR/crt0-efi-x86_64.o"

build_one() {
    local name="$1"
    local file="$SRC/$name.c"
    if [ ! -f "$file" ]; then
        echo "skip  $name (src/$name.c missing)"
        return 1
    fi
    echo "build $name"
    gcc $CFLAGS -c "$file" -o "$OBJ/$name.o" || exit 1
    ld $LDFLAGS "$OBJ/$name.o" -o "$OBJ/$name.so" -lefi -lgnuefi || exit 1
    objcopy -j .text -j .sdata -j .data -j .rodata -j .dynamic -j .dynsym -j .rel -j .rela -j '.rel.*' -j '.rela.*' -j .reloc --target efi-app-x86_64 "$OBJ/$name.so" "$BIN/$name.efi" || exit 1
    return 0
}

HAVE_ALL=1
build_one Bootloader || HAVE_ALL=0
build_one Kernel || HAVE_ALL=0
build_one Command || HAVE_ALL=0

if [ "$HAVE_ALL" -ne 1 ]; then
    echo "Binaries built where sources exist. ISO skipped until all components exist."
    exit 0
fi

for t in mkfs.fat mmd mcopy xorriso; do
    if ! command -v "$t" >/dev/null 2>&1; then
        echo "missing tool: $t (dosfstools, mtools, xorriso)"
        exit 1
    fi
done

rm -rf "$ISO_DIR"
mkdir -p "$ISO_DIR"

dd if=/dev/zero of="$IMG" bs=1M count=$IMG_MB status=none
mkfs.fat -F 32 -S 512 -s 1 -n IZHOS "$IMG" >/dev/null

mmd -i "$IMG" ::/EFI ::/EFI/BOOT ::/EFI/IzhOS ::/IzhOS ::/IzhOS/System
mcopy -i "$IMG" "$BIN/Bootloader.efi" ::/EFI/IzhOS/IzhOS.efi
mcopy -i "$IMG" "$BIN/Bootloader.efi" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$IMG" "$BIN/Kernel.efi" ::/IzhOS/System/Kernel.efi
mcopy -i "$IMG" "$BIN/Command.efi" ::/IzhOS/System/Command.efi

xorriso -as mkisofs -R -J -V IZHOS -e efiboot.img -no-emul-boot -isohybrid-gpt-basdat -o "$DL/IzhOS.iso" "$ISO_DIR" >/dev/null 2>&1

echo "done  $DL/IzhOS.iso"
