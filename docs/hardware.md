# Hardware of the tags (BG22Pin / pin16)

Read from real tags with the "hardware card" of the diagnostic build (`docs/DIAG.md`): EL016F5W4C, EL016F6W4A and EL022F5C4C.
All three have the same board wiring; they differ in the panel only. Russian version: `hardware.ru.md`.

## Chip and memory
- EFR32BG22C224F512IM40 (Cortex-M33, 512 KB flash, 32 KB RAM), CR2450 cell, one 38.4 MHz crystal, no 32 kHz crystal (the sleep clock is the LFRCO in precision mode).
- Solum bootloader (Gecko Bootloader 2.4.0, SPI flash single slot) at 0x0..0x6000, application from 0x6000.
- External flash: Fudan, JEDEC A1 28 13 on the tag we dumped first; the EL022 tag (160 x 296) reports JEDEC 85 20 13 (manufacturer code 0x85, Puya as far as we know; check the marking), also 512 KB, SFDP table of 9 dwords. Layout on both: 4 KB sectors, 256-byte pages, 0x00000..0x3B000 bootloader update slot, 0x3C000..0x7CFFF slides, 0x7D000/0x7E000 settings, 0x7F000 self-test sector.

## Pins (same on all three tags)
| Function | Pin |
|---|---|
| Panel SPI (USART1, mode 0, 5 MHz, no MISO) | SCK PA4, MOSI PA3 (this is also the SWO pin: keep the debug unit off it) |
| Panel control | CS PB0, DC PA6, RST PA7, BUSY PA8 (low = busy), EN PA0 (panel supply, high = on) |
| External flash (USART0) | SCK PC2, MOSI PC1, MISO PC0, CS PC3 |
| NFC chip TNB132M (I2C0) | SDA PD3, SCL PD1, PWR PD0 (supply for I2C access), FD PD2 (field detect, high = a reader is near) |
| Buttons (active low) | PB1, PA5 (which one is on the case differs between models) |
| LED (active low) | PC5 red, PC6 green, PC7 blue, as the eye sees them |
| Debug UART (EUART) | TX PB2, RX PB3 |
| Not connected | PB5..PB8 (the generated debug configuration points at them: disabled at start-up) |

## Factory data page ("userdata", Solum)
Read from the page at `USERDATA_BASE`: byte 0x09 controller type, 0x0A colour type, 0x0B/0x0D resolution x/y (little endian), 0x12/0x13 capability bits, 0x16 tag type.

| Tag | controller | resolution as stored | tag type | panel used by the firmware |
|---|---|---|---|---|
| EL016F5W4C, EL016F6W4A | 0x1D (an older EL016: 0x37; 0x1C is treated alike) | 200 x 200 | 0x73 | EL016: 200 pixels per line, 200 lines, 50 bytes per line, 10 000 bytes per frame |
| EL022F5C4C | 0x1E | 296 x 160 | 0x74 | EL022: scans 160 pixels per line, 296 lines, 40 bytes per line, 11 840 bytes per frame |

Any other combination: the tag works over Bluetooth, but the firmware does not draw (it does not know the controller). The page says so.

## Panel (JD-type controller, 2 bits per pixel)
Pixel codes: 0 black, 1 white, 2 yellow, 3 red; first pixel in the top bits of the first byte. A line is sent after command `10` with chip select held low. Every sequence below was recorded from a real tag and is replayed byte for byte by the PC test (`tests/host/test_panel.c`).

EL016 (waits in ms):
`W20, power on, W100, [W200, RST low, W40, RST high, W200]`, then `00 0F 09; 01 07; 06 40 00 00; 61 00 C8 00 C8; 50 37; 30 08; E7 1C; E9 01; FF A5; EF 03 2C 07 20 07 10 09 21; DC 01; DD 06; DE 46; DA 08; E8 00; FF E3`, `10` + 200 lines of 50 bytes, `W10, 04 00, W100, BUSY high (max 1 s), W10, 12 00, W300, BUSY high (max 50 s, about 23 s), W100, 02 00, W100, BUSY high (max 2 s), 07 A5, W100`, release.
The slide line y is panel scan line y on EL016 (stored as is; v20 - v19 had lines and pixels reversed, which turned the EL016 splash and slides by 180 degrees, checked on a tag 16 Nov 2026; v18 came out mirrored left to right). One base transform per panel is shared by the splash, the firmware test pictures and the slides; the orientation setting is applied on top of it for every stored format.

EL022: two reset pulses (same shape), `W10`, then `4D 78; 00 07 09; 01 07; 03 10 54 44; 06 0F 0A 2F 25 22 2E 21; 50 37; 60 02 02; 61 00 A0 01 28; E7 1C; E3 22; B4 D0; B5 03; E9 01; 30 08`, `W300`, `10` + 296 lines of 40 bytes, the same tail as EL016 (refresh about 20.7 s). Slide line y is scan line y.
EL022 (v19, unchanged in v20): lines in reverse order, pixels not reversed (the v18 splash came out upside down). **Confirmed on a 160 x 296 tag, 16 Nov 2026** (splash, slides). EL016 changed in v20, not yet checked on a tag: upload the "Orientation test" picture (page, Settings: a big F and a red arrow up) and draw it together with firmware test picture 8; F must read upright on both. If it does not, the orientation setting turns it.

## NFC chip (3ALogics TNB132M, Type 3 tag with an I2C side)
The chip is powered over PD0 for I2C access (about 40 ms to wake up), then:
- device 0x30: write `21 04`, read register `25` (59 on all tags seen); device 0x43: read block 0x30 (wake-up exchange)
- device 0x48, sub-address 0: attribute information block, 16 bytes: `10 02 01 00 3C ...`, RWFlag at byte 10, Ln (3 bytes) at 11..13, checksum (16-bit sum of bytes 0..13) at 14..15
- device 0x40: NDEF blocks of 16 bytes at sub-addresses 0x10, 0x20, ...; the stock firmware reads an attribute block before every data block, the firmware does the same
- the record is a short URI record `D1 01 len 55 prefix text`
Writing (BLE `4F 18` + URL) writes the attribute block, then the data blocks (10 ms apart), then reads everything back and compares. Not yet tested on all tags.
Port C and D pins cannot wake the chip from EM2 with an interrupt: while it sleeps, the field detect and supply pins are looked at every 200 ms.
