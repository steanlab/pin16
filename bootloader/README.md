# Bootloader of the pin16 tags

A Silicon Labs Gecko Bootloader for the EFR32BG22C224F512IM40 of the Solum tags (EL016 1.6", EL022 2.2"): the update image (a GBL file, LZMA compressed,
no signature, no encryption) is stored by the firmware in slot 0 of the external SPI flash (address 0, 236 KB) and installed after a restart
(`46 04 A5`, see `docs/PROTOCOL.md`). The flash type is read from its SFDP table.

* `BG22Badge_BTL.slcp` – the project (Gecko SDK 4.4.1 components, one slot).
* `btl_spi_controller_usart_driver_cfg.h` – pins of the flash: USART0, MISO PC0, MOSI PC1, SCK PC2, CS PC3, 6.4 MHz.
* `patch_sfdp.py` – build-time patch of the SDK file `btl_storage_spiflash_sfdp.c`: the SFDP table of the Fudan flash has unusable values (erase times 0xFF,
  page size); three guards and the word size. No SDK code is stored in this repository.
* `patch_id.py` – writes the variant id byte `03` into the version word of the bootloader's info struct (the firmware shows it: `4F 0D`, `BTL … id=03`).
* `build_bootloader.sh` – the build, called by `../build_all.sh`; result `variant_binaries/SOLUM_BTL.s37`. The CI joins it with the firmware into `…_FULL_vN.s37`.

The bootloader sits at 0x0..0x5FFF, the application starts at 0x6000, the bootloader's own storage (tokens) is at the end of the flash.
A tag that was unlocked with `commander device unlock` has no bootloader; the `…_FULL_…s37` image brings it. See `docs/USER_GUIDE.md`, "First install on a factory tag".

Status v19: built by CI, **not yet tested on a tag** (owner flashes one tag over SWD and checks: boot, slide, OTA).
