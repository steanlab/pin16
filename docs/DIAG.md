# Diagnostic build ("hardware card")

Russian version: `DIAG.ru.md`.

The workflow builds two firmware images: the normal one and one with `-DBG_DIAG` (files `*_diag*`, `pin16_vN_diag.ota.bin`).
The diagnostic build adds `firmware/hwcard.c`: a trace of everything that goes over the panel bus and the NFC bus after power-up, plus a card with what the tag knows about itself. The normal build has no trace code and no trace memory (`hwcard.h` turns the hooks into empty statements).

## Use it on a tag you want to support
1. Flash or install the `_diag` image (OTA works too: touch the tag with the phone first). The panel draws the boot splash; the trace records that draw.
2. Connect with the page, scroll to the bottom, open "Advanced", then "For developers", and press **Hardware card (diag build)** (v20; `4F 0F`) or **ID / reset** (`4F 0D`). The tag prints the card and the trace as text lines. Use "Copy log" on the page.
3. `4F 0F 00` clears the trace: the next panel draw (for example a test picture) is recorded.
4. Send the log (all lines: `HW`, `PANEL`, the pin lines, `NFCCHIP`, `NFCAI`, `NFCTEXT`, `GPIN`, `UD`, `FLASH`, `SFDP` and the `TR` lines) with the photos of the board front and back.

## Card lines
- `HW tag=.. ctrl=.. col=.. res=WxH capa=..`: factory data page; `PANEL EL016/EL022/? ok=.. WxH mirror=..`: what the firmware chose
- pin lines `SCK= MOSI= CS= DC= RST=`, `BUSY= EN=`, `FLASH`, `NFC`, `IO` (buttons, LED), `DBG`; `GPIN`: levels of all pins of ports A..D now
- `NFCCHIP present= reg25= ln=`, `NFCAI` (attribute block), `NFCTEXT kind= cap= text` (what the chip holds: kind 1 link, 2 text, 3 Wi-Fi network name)
- `UD 00:` .. `UD 70:`: first 128 bytes of the factory data page
- `FLASH JEDEC aa bb cc` and `SFDP 00:`, `SFDP 10:` and four more `SFDP xx:` lines (v20; v19 printed the first 128 bytes, which missed the basic table on the Fudan flash, where it starts at 0x80): JEDEC id, the SFDP header (32 bytes) and 64 bytes of the basic table at the pointer of header 0, of the external flash, read with command `5A`. The build-time patch of the bootloader (`bootloader/patch_sfdp.py`) exists because of unusable values in this table; send these lines from every tag type so that the need can be checked per chip
- `TR n t type n=total b=blocks: bytes`: trace records, `t` ms since the first record. Types: `C` command (first byte = command), `D` data (pixel stream: first bytes, total length and number of blocks), `X` read, `N` reset pin level change (the byte is the pin levels: bit 0 BUSY, bit 1 RST, bit 2 DC), `W` wait ms, `B`/`b` start / end of a BUSY wait, `A` power on, `Z` released (levels before release), `I` NFC I2C exchange (address, flags, ok, lengths, first bytes).
The trace buffer is 2 KB and fills once; NFC records may use only its first part.

## Cost
The diagnostic image needs 2 KB of RAM more than the normal one and is a few KB bigger. If it does not fit on a tag, reduce `SL_HEAP_SIZE` for that build only.
