#!/bin/bash
# Builds the bootloader of the tags with Silicon Labs slc and arm-none-eabi-gcc (inside the Docker image, see ../Dockerfile).
# Result: variant_binaries/SOLUM_BTL.s37 (id byte 03). Needs python3.
set -e
GSDK_PATH=${GSDK_PATH:-/gecko_sdk/}
rm -rf out base_binaries variant_binaries
mkdir base_binaries variant_binaries
slc --daemon generate -s "${GSDK_PATH}" -p BG22Badge_BTL.slcp -d ./out -o makefile -cp
cd out
python3 ../patch_sfdp.py gecko_sdk_4.4.1/platform/bootloader/storage/spiflash_sfdp/btl_storage_spiflash_sfdp.c
make -f BG22Badge_BTL.Makefile -j8
cp build/debug/BG22Badge_BTL.s37 ../base_binaries/BG22Badge_BTL.s37
cd ..
python3 patch_id.py base_binaries/BG22Badge_BTL.s37 variant_binaries/SOLUM_BTL.s37 3
arm-none-eabi-size out/build/debug/BG22Badge_BTL.out
SZ=$(arm-none-eabi-size out/build/debug/BG22Badge_BTL.out | awk 'NR==2 {print $1 + $2}')
echo "bootloader image: $SZ bytes of 24576 (0x6000)"
[ "$SZ" -le 24576 ] || { echo "the bootloader does not fit below the application (0x6000)"; exit 1; }
