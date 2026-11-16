#!/bin/bash

# the repository is owned by another user inside the container: without this git refuses ("dubious ownership") and the firmware version string stays empty
git config --global --add safe.directory "$(pwd)" || true

# Build the bootloader (bootloader/build_bootloader.sh, result bootloader/variant_binaries/SOLUM_BTL.s37)
# A failed bootloader build does not stop the firmware build (then there are no ..._FULL_... images)
(cd bootloader && bash build_bootloader.sh) || echo "=== BOOTLOADER BUILD FAILED, continuing with the firmware"

# Build firmware: first the diagnostic build (-DBG_DIAG, docs/DIAG.md), then the normal one. The normal build is built last,
# so firmware/out holds its files (map, memory report) for the later steps.
cd firmware
BG_EXTRA_DEFS=-DBG_DIAG BG_OUT=firmware_diag.s37 source build_firmware.sh || exit 1
echo "=== size of the diagnostic build"; arm-none-eabi-size out/build/debug/BG22Badge.out
cp out/build/debug/BG22Badge.out firmware_diag.out
BG_EXTRA_DEFS= BG_OUT=firmware.s37 source build_firmware.sh || exit 1
echo "=== size of the normal build"; arm-none-eabi-size out/build/debug/BG22Badge.out
cd ..

# firmware version of this project (firmware/fwver.h): names the output files, equals the page version
THIS_VER=$(awk '/define BG_FW_VERSION/ {print $3}' firmware/fwver.h | tr -d '\r')

# Assemble bootloader + firmware all-in-one images: for a tag that was unlocked (no bootloader on it) and for the first install over SWD
mkdir ./full_binaries
mkdir ./full_binaries/v$THIS_VER
for filename in bootloader/variant_binaries/*.s37; do
  [ -f "$filename" ] || continue
  commander convert -o ./full_binaries/v$THIS_VER/$(basename "$filename" .s37)_FULL_v$THIS_VER.s37 $filename firmware/firmware.s37
  commander convert -o ./full_binaries/v$THIS_VER/$(basename "$filename" .s37)_FULL_v${THIS_VER}_diag.s37 $filename firmware/firmware_diag.s37
done

mkdir ./ota_files
# product mark in the unencrypted metadata of the update file: the tag refuses a file without it (firmware/ota.c)
printf 'BG22BADG' > firmware/bg22_fwid.bin
commander gbl create ./ota_files/pin16_v"$THIS_VER".ota.bin --app firmware/firmware.s37 --compress lzma --metadata firmware/bg22_fwid.bin
commander gbl create ./ota_files/pin16_v"$THIS_VER"_diag.ota.bin --app firmware/firmware_diag.s37 --compress lzma --metadata firmware/bg22_fwid.bin
