# pin16

Open firmware and a control page for small e-paper tags that look like badges: **load pictures over Bluetooth LE from a web page, show them as a slideshow**, no SWD flashing for every picture. Russian version: `README.ru.md`.

Supported tags (EFR32BG22C224F512IM40, CR2450, NFC, external 512 KB SPI flash):

| Tag | Panel | Size | Colours |
|---|---|---|---|
| Solum EL016F5W4C, EL016F6W4A | EL016 | 200 x 200 | black, white, yellow, red |
| Solum EL022F5C4C | EL022 | 296 x 160 (scanned as 160 x 296) | black, white, yellow, red |

Other tags with the same board but another controller work over Bluetooth and report themselves, but the firmware does not draw on them until their panel sequence is added (`docs/DIAG.md` shows how to record it).

## Features
- Pictures converted in the browser (presets, dithering, zoom, rotate), up to 32 slots in the external flash, raw / LZ4 / XOR+LZ4 storage, previews read back from the tag
- Slideshow on a timer, periodic clean, tap = next picture, boot splash
- Firmware update over Bluetooth (OTA) with checks; dangerous commands only within 60 s after touching the tag
- Write a link, a short text or Wi-Fi access into the NFC chip (read back and checked; the Wi-Fi password is readable by anyone who touches the tag)
- LED signals, optional BTHome beacon, locator patterns, battery and watchdog guards
- Optional diagnostic build: hardware card (pins, factory data, recorded panel and NFC bus traffic)
- Progress panel fixed at the top of the page (percent, bytes, time) for uploads, firmware update and previews; orientation test picture
- A subset of the OpenDisplay protocol (service 0x2446) next to the own protocol, documented in `docs/PROTOCOL.md` for third-party clients
- English and Russian page, works offline as an app (PWA)

## Repository
- `firmware/` own code (hardware layer, panel and NFC drivers, slots, Bluetooth, OTA, LED), `firmware/BG22Badge.slcp` project for the Silicon Labs Gecko SDK 4.4.1
- `docs/` control page (`index.html`, `sw.js`, manifest, icons)
- `tests/host/` tests of the modules on a PC (simulated flash, panel recorder, virtual time, AddressSanitizer)
- `bootloader/` own Gecko bootloader project for the tags (SDK sample components, pins, build-time patch of the flash driver); no SDK code inside
- `docs/` user guide, protocol (`PROTOCOL.md`), hardware (pins, panels, NFC), diagnostic build
- `Dockerfile`, `build_all.sh`, `.github/workflows/main.yml`: build in GitHub Actions; artifacts: `*_FULL_vN.s37` (bootloader + firmware in one image), `ota_files/pin16_vN.ota.bin`, `firmware.s37`, `_diag` variants

## Install
* A tag that already runs this firmware (or an earlier one with a bootloader): update over Bluetooth, page card "Firmware update", file `pin16_vN.ota.bin`.
* A **factory tag**: it is locked; unlocking it over SWD erases the factory firmware for good (there is no way to back it up), and the tag has no bootloader afterwards. Flash the all-in-one image `SOLUM_BTL_FULL_vN.s37` (bootloader + firmware). Step by step in `docs/USER_GUIDE.md`, "First install on a factory tag".
* A tag with the bootloader already in place (earlier project, unlocked): flash `firmware.s37` from address 0x6000 over SWD, or update over Bluetooth.

## Licence
Copyright (c) 2026 Siarhei Besarab (steanlab). Source code, page and documents: GNU GPL version 3 or later, see `LICENSE`. The ready-made firmware files in the Releases are offered on separate terms (MIT for the pin16 code, Silicon Labs licence for the SDK parts, for Silicon Labs devices only), see `LICENSE-RELEASES.md`. `NOTICE.md` says what is original and what is not included. The protocol is documented in `docs/PROTOCOL.md`.
