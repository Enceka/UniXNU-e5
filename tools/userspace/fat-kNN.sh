#!/bin/sh
# Fresh-build the payload FAT for kNN: kernel first (contiguous), then the rest.
# Usage: sh fat-kNN.sh <kernelpath> <adtpath> [workdir]
set -e
K=$1; A=$2
IMG=/Volumes/Projects/e5-xnu/uefi/build/e5-payload-fat.img
EFI=/tmp/e5uefi/Build/e5Pkg/DEBUG_CLANGDWARF/AARCH64
SEED=/tmp/seed.dbr

# 1. fresh 56MiB FAT16 image
dd if=/dev/zero of="$IMG" bs=512 count=114688 2>/dev/null
DEV=$(hdiutil attach -nomount "$IMG" | tr -d ' ')
newfs_msdos -F16 -b512 -c4 -r1 -n2 -e512 /dev/r${DEV#*/dev/}
diskutil mountDisk $DEV

# 2. kernel FIRST (contiguity), then the rest
cp "$K" "/Volumes/NO NAME/kernel.macho"
cp "$A" "/Volumes/NO NAME/kernel.macho.adt"
cp /tmp/kernel.macho.trustcache "/Volumes/NO NAME/kernel.macho.trustcache"
cp "$SEED" "/Volumes/NO NAME/seed.dbr"
cp "$EFI/E5BootMenu.efi" "/Volumes/NO NAME/E5BootMenu.efi"
cp "$EFI/DarwinBoot.efi" "/Volumes/NO NAME/DarwinBoot.efi"
printf 'echo E5-STARTUP-NSH from FV\nfs2:\\E5BootMenu.efi\n' > "/Volumes/NO NAME/startup.nsh"
printf 'E5-PAYLOAD-OK' > "/Volumes/NO NAME/E5-PAYLOAD-OK.txt"
rm -f "/Volumes/NO NAME/._"*
diskutil eject $DEV
echo "FAT done: $IMG"
