# Notice: what is original and what is not

**Original (GNU GPL v3 or later, see `LICENSE`; the ready-made files of the Releases are on the terms of `LICENSE-RELEASES.md`)**: everything in `firmware/` (hardware layer `hw.*`, NFC `nfc.*`, panel `panel*.c`, drawing `draw.*`, slides `slot.*`, `lz.*`, `pics.*`, Bluetooth `ble_badge.c`, commands, OTA, LED, beacon, settings, ...), `web/`, `tests/`, `docs/`.
Since v18 the firmware contains no code of the OpenEPaperLink firmware: the hardware layer, the panel driver (two profiles) and the NFC driver were written from the recordings of real tags (`docs/hardware.md`). The project began as a fork of the slideshow branch of OpenEPaperLink/Tag_FW_EFR32xG22 (CC BY-NC-SA 4.0) and kept that code until v17.

**Not in this repository, used at build time**: Silicon Labs Gecko SDK 4.4.1 (its own licence; the build downloads it), Simplicity Commander and `slc` (downloaded by the `Dockerfile`).

**Other third-party parts**: `firmware/sl_iostream_eusart_euart_debug_config.h` is a Silicon Labs configuration header (Zlib licence, header kept); `firmware/font7x14.c` is generated from DejaVu Sans Mono Bold (Bitstream Vera licence, free to embed).

**Bootloader**: `bootloader/` holds our own project files (component list, pins of the flash, two small build-time patch scripts, README; GPL v3 or later). The code of the Gecko Bootloader itself is Silicon Labs' (Gecko SDK 4.4.1, its own licence); the build downloads the SDK, nothing of it is stored here, and `patch_sfdp.py` changes the SDK file `btl_storage_spiflash_sfdp.c` only in the build directory (three guards against unusable SFDP values of the Fudan flash and the word size of the part). The files of the earlier OpenEPaperLink bootloader folder (CC BY-NC-SA 4.0) are not part of this repository since v19.
A factory tag is locked: the first install erases its factory firmware and brings the bootloader with the all-in-one image (`docs/USER_GUIDE.md`); a tag that already has a bootloader is updated over Bluetooth.

**Dockerfile**: the build image recipe follows the one of the OpenEPaperLink fork, which credits NabuCasa/silabs-firmware-builder (comment kept in the file). It contains no project code; it downloads Simplicity Commander and `slc` from release files of a repository of the owner. Before the public release the owner decides whether to keep this recipe or write a new one (open item).

**Ready-made files (Releases)**: they contain the pin16 code linked with the Silicon Labs Gecko SDK, whose Master Software License Agreement allows object code only inside complete applications and only for Silicon Labs devices, and forbids making the SDK subject to an open source licence. Therefore the files are offered on the terms of `LICENSE-RELEASES.md` (MIT for the pin16 code, the Silicon Labs licence for their parts) and not under the GPL; the GPL applies to the source of this repository.
