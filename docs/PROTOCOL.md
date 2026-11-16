# Bluetooth protocol of the tag (firmware v19)

For people who write their own client (an app, a script, Home Assistant). Everything here is read from the firmware sources
(`firmware/ble_badge.c`, `cmd_set.c`, `slot.c`, `ota.c`, `nfc.c`, `od.c`); the reference client is the page `docs/index.html`.
Marks: **[OK]** checked on a real tag, **[code]** read from the code and covered by the host tests, not checked on a tag with a third-party client,
**[?]** not verified.

## 1. Transport

| Item | Value |
|---|---|
| Own service | `b9220000-5c1e-4f6a-9d3b-0a7c6e1f2d40` |
| `rx` (client → tag) | `b9220001-…` write and write without response, up to 244 bytes |
| `tx` (tag → client) | `b9220002-…` notify (and read), up to 244 bytes |
| OpenDisplay service | `0x2446`, one characteristic `0x2446` (read, write, write without response, notify), see section 9 |
| Notifications | the client must enable them on `tx` first; one notification carries at most MTU − 3 bytes |
| MTU | 23 until the client's Bluetooth stack negotiates more (the tag takes whatever it gets). Chunk sizes below follow the MTU |

The tag is only connectable in a **window**: after power-up, after an NFC touch or a button press (`wt` seconds, default 60), and after a
link ends. The window is shown by a blue flash every 5 s; when it is over the tag flashes blue twice. In OpenDisplay mode (setting `mode`
= 1) the tag stays connectable all the time. The tag closes a link that does nothing for a while.

Two kinds of messages go from the tag to the client, told apart by the first byte:

* **text line**: first byte printable ASCII (0x20..0x7E), UTF-8 text without a line end, at most 96 bytes. Most answers are text lines.
* **binary**: first byte ≥ 0x80 (`cmd | 0x80` answers, section 3), `0xC0` slot data (section 5), or `0x00` OpenDisplay answers (section 9).

While an OpenDisplay client is on the link, text lines are not sent at all.

**Tap gate.** Commands that replace the firmware (OTA begin), rewrite the NFC chip, change the name, the mode or the windows are accepted only
within 60 s after someone touched the tag with a phone (NFC field) or pressed a button. Otherwise the answer is
`ERR tap: touch the tag with the phone, repeat` (OTA: `OTA FAIL tap: …`). `4F 19` tells the state. Slides, ordinary settings and reading are not gated.

## 2. Panels and pictures

| Tag | Panel | Pixels (x × lines) | Frame bytes |
|---|---|---|---|
| Solum EL016F5W4C / F6W4A | EL016 | 200 × 200 | 10 000 |
| Solum EL022F5C4C | EL022 | 160 × 296 (scanned) | 11 840 |

`4F 1A` answers `PANEL <name> <w>x<h> mirror=<0|1> ctrl=<hex> tag=<hex>` (`PANEL ? 0x0 …` on an unknown panel: the tag works over Bluetooth, but cannot draw).

A **frame** has 2 bits per pixel, four pixels per byte, the **leftmost pixel in the top two bits**, rows from the top, no padding:
`0` black, `1` white, `2` yellow, `3` red. A frame is `w*h/4` bytes. This is the format the page sends and the firmware stores; the firmware
turns it into the panel's scan order itself. The firmware has one base transform per panel, so that a frame drawn upright on the page
is upright on the tag; the setting `orientation` (`4D`) turns the picture on top of that: bit 0 flips vertically, bit 1 flips horizontally,
for every stored format. The base transform of v19 is **[?]** until the "orientation test" button of the page has been seen on both tags.

## 3. Binary commands

Request: `cmd args…` in one write. Answer: `cmd|0x80, status, data…` (status `00` ok, `FE` wrong length, `FF` unknown command).

| Cmd | Meaning | Answer data |
|---|---|---|
| `01` | ping | `PONG` |
| `02` | info | fw version (u16 LE), battery mV (u16 LE), tag type (u8), last close reason (u16 LE) |
| `03 …` | echo | the same bytes |
| `04 b` | LED test, one byte of colour bits | – |
| `10` | external flash JEDEC id | 3 bytes (status 01 = flash does not answer) |
| `11 a2 a1 a0 n` | read n ≤ 50 bytes of the external flash | n bytes |
| `12` | erase/program/verify self test of the last sector | erase ms, program ms, bad index (u16 LE each) |

## 4. Slots

32 slots (0..31) in the external flash, 4 KB sectors, area 0x3C000..0x7CFFF; each slot is a header plus a stream. Stream formats
(`fmt`): `0` raw frame, `1` an LZ4 block stream of the frame, `2` every line XORed with the line above, then LZ4 (offsets ≤ 2048). All answers are text lines.

| Command | Meaning | Answers |
|---|---|---|
| `40` | list | one `slot N: fmt F, L bytes, S sectors, crc=XXXXXXXX` per used slot, then `slots: U used, F of T sectors free, largest run R, max 32 slots` |
| `41 N` | erase slot N (`41 FF` = all) | `ERASED slot N` / `ERASED all` / `ERR busy` |
| `42 N len32 crc32 win fmt` | start an upload to slot N: stream length, CRC-32 (zlib) of the stream, chunks per acknowledgement (1..32, default 8), format | `UP READY chunk=C win=W mtu=M rx=244 fmt=F sect=S` or `UP FAIL <why>` (`short`, `slot`, `panel`, `fmt`, `len`, `flash`, `erase`) or `slot N: no room …` |
| `43 seq16 data…` | one chunk (C bytes at most), `seq` counts from 0 | `UP ACK <bytes so far>` after every `win` chunks and after the last; `UP FAIL seq A expected B`, `UP FAIL <why>` |
| `44` | end of the upload | `UP OK slot N crc=XXXXXXXX` or `UP FAIL crc` |
| `49` | abort the upload | `UP ABORT` / `UP idle` |
| `45 N` | draw slot N now (about 25 s) | `DRAW slot N`, then `DRAW done` / `DRAW fail` / `DRAW no slot` |
| `4C` | draw the next slide | as `45` |
| `47 N` | verify the stored slot (CRC and decoding) | `VERIFY slot N ok crc=…` / `FAIL crc=…` / `FAIL decode` / `empty` |
| `48` | MTU | `MTU mtu=M rx=244` |

Raw upload length must be exactly the frame size of the panel. Compressed streams are checked while decoding (exactly `lines` lines).
The page sends `win` chunks and waits for `UP ACK` before the next group. While a draw runs the tag does not answer other commands (the radio is
shared with the panel driver); wait for `DRAW done`.

`4F 13 N` reads slot N back (thumbnails): `DUMP N fmt F len L crc=…`, then binary frames `C0 N seq16 data…` (up to 90 bytes of the stored stream), then `DUMP done N`.

## 5. Slideshow and settings

All answers are text lines; settings are saved in the tag (two flash sectors, a new copy for every change).

| Command | Meaning | Answer |
|---|---|---|
| `4B` | read the slideshow | `SS en=E every=S mask=HEX poll=O clean=N/M cur=C next=X` |
| `4A en every_lo every_hi mask0 order [clean_n clean_mode [mask1 mask2 mask3]]` | slideshow: on/off, interval in minutes (1..1080), slide mask (bit per slot, `mask0` = bits 0..7 …), order 0 sequential / 1 by number, full redraw every `clean_n` slides in mode 0/1/2 | `SS …` |
| `4D mode slot [lut]` | orientation 0..3 (section 2), slot for OpenDisplay (0..31), colour table byte (`E4` = unchanged) | `OD mode=M slot=S lut=XX` |
| `4E name…` | name tail, 1..5 printable characters (empty = default) **(tap gate)** | `NAME saved, reboot (4F A5) to use it` |

`4F <sub> …` sub-commands:

| Sub | Meaning | Answer |
|---|---|---|
| `01 n` | transmit power 0..5 (−15 … +6 dBm) | `TX n` |
| `03` | system state | `SYS tx= wd= led= v= mode= wt= wb= tap= sw= ct= tc=` |
| `04 n` | LED level 0..3 | `LED n` |
| `05 m` | mode 0 standalone, 1 OpenDisplay, restarts **(tap gate)** | `MODE m, restarting` |
| `06 wt wb` | Bluetooth windows after a touch / at boot, 10..250 s **(tap gate)** | `WINDOWS tap= boot=` |
| `08 n` | 1: a touch of the tag also draws the next slide, 0: it does not | `TAP next=n` |
| `09` | event log | `EVENTS n`, then `EV t name a b` lines |
| `0A [1]` | close the link (1: and the window) | – |
| `0B` | stored settings | `CFG or=<orientation> od=<slot> lut=XX nm=<name>` |
| `0C [en itv mask [win]]` | BTHome beacon, read or set | `BT en= itv= mask= win=` |
| `0D` | chip id, reset cause, bootloader, stack | `ID …`, `RST rc= fw=`, `BTL main= props= id=`, `STK free= of=` |
| `0E` | battery | `BAT mv= min= ever= pct= low= draws=` |
| `10` | panel temperature probe | `PT nC` / `PTEMP n/a` |
| `11 k` | a picture made by the firmware: 1 splash, 2 bars, 3 white, 4 black, 5 yellow, 6 red, 7 checker, 8 frame with F and UP; `20` = clean cycle (2 min) | `PIC k start` / `PIC k done` / `CLEAN …` |
| `12 [off]` | boot splash on/off | `SPLASH off=n` |
| `14 r g b [t]`, `15 …`, `16 …`, `17 …` | LED tests, locator beacon, amber colour | `LEDT`, `LEDP`, `BEACON …`, `AMBER g=` |
| `18`, `1B`, `19` | NFC chip, section 7 | |
| `1A` | panel | `PANEL …` (section 2) |
| `A5` | restart | `REBOOT` |

Errors are `ERR short`, `ERR range`, `ERR busy`, `ERR tap: …`, `ERR cmd 4F xx` (unknown).

## 6. Firmware update (OTA)

The new firmware is a GBL file (the build artifact `…ota.bin`) with the product mark `BG22BADG`. It is written into the bootloader slot
(external flash 0..0x3B000) and installed by the Gecko bootloader after a restart.

| Command | Meaning | Answer |
|---|---|---|
| `46 00` | status | `OTA st= got= len= btl= cap= room= fw= minmv=` |
| `46 01 len32 crc32 win` | begin (**tap gate**; length 64..0x3B000, CRC-32 of the whole file, chunks per acknowledgement); erases the old image | `OTA READY chunk=C win=W mtu=M` or `OTA FAIL <why>` |
| `46 02 seq16 data…` | chunk | `OTA ACK <bytes>` per `win` chunks; `OTA FAIL seq A expected B` |
| `46 03` | verify: length, CRC-32 read back from the flash, GBL header, End tag CRC, product mark | `OTA OK len= crc= gbl=1 product=1` or `OTA FAIL incomplete / crc / no end tag / not a gbl file / wrong product / readback / gbl crc` |
| `46 04 A5` | install: restart into the bootloader (needs a verified image and a battery of at least `minmv`) | `OTA INSTALL bootloader takes over, reconnect in about 40 s` |
| `46 05` | abort | `OTA ABORT` |

The tag does not answer for about 40 s after the install; the new `Info` shows the new firmware version.

## 7. NFC chip

The chip (TNB132M, NFC Forum Type 3 over I²C) holds one NDEF record. All writes need the tap gate; the chip is read back after writing.
Limits: URL up to 120 bytes of printable ASCII, text up to 120 bytes of valid UTF-8, Wi-Fi network name up to 32 bytes, key up to 63 bytes; and not more than the chip holds (`cap` blocks of 16 bytes).

| Command | Meaning |
|---|---|
| `4F 18` | read; `4F 18 url…` writes a link (old form) |
| `4F 1B 00 url…` | write a link (URI record) |
| `4F 1B 01 text…` | write a text (Text record, language `en`, UTF-8) |
| `4F 1B 02 auth ssid 00 key` | write Wi-Fi access (MIME `application/vnd.wfa.wsc`, credential TLVs); `auth` 0 open, 1 WPA2-personal, 2 WPA/WPA2-personal; open network: empty key. **The key is stored readable by anyone with a phone.** |
| `4F 19` | gate state: `TAP open=1 left=NNs` |

Answers: `NFC written` or `NFC ERR n` (1 bus, 2 argument or too big for the chip, 3 verify, 4 busy), then always
`NFC present=P r25=R len=L cap=C kind=K` (kind 0 none, 1 link, 2 text, 3 Wi-Fi, 4 other), `NFC text=<what is stored: link, text or the network name, never the key>` and,
when longer than 80 bytes (cut between two characters), `NFC text+=<rest>`.

## 8. Diagnostic build

A build with `-DBG_DIAG` (built together with the normal one, files `…_diag`) adds `4F 0F` (hardware card: pins, NFC answers, factory data page, the first 128 bytes of the
SFDP table of the external flash with its JEDEC id, a trace of the panel and NFC buses). See `docs/DIAG.md`.

## 9. OpenDisplay compatibility

The tag speaks a subset of the OpenDisplay protocol (opendisplay.org, "OpenDisplay", reference clients `py-opendisplay`, Home Assistant integration `opendisplay`).
Spec pages used: protocol overview, communication protocol (BLE flow), display data format. Not tested with a real third-party client yet **[?]**.

What the tag does:

* **GATT**: service `0x2446` with one characteristic `0x2446` (read, write, write without response, notify). The client enables notifications and writes
  messages `00 cmd …`; answers come as notifications `00 cmd data` (success) or `FF cmd` (error) on the same characteristic. The same messages also work on the own `rx`/`tx` pair.
* **Advertising** (only in OpenDisplay mode, `4F 05 01`): flags, service `0x2446`, manufacturer data with company id `0x2446` and a 16-byte record (the record that `00 44` returns);
  scan response: name `OD` + the last three address bytes in hex. Without OpenDisplay mode the tag advertises its own name `pin16-…` in its windows.
* **Commands**: `00 40` read configuration (framing: length, version 1, packets, CRC-16/CCITT-FALSE over everything except the CRC with the length bytes zero;
  chunks `00 40 idx16 [total16] data`); packets sent: system (1), manufacturer (2), power (4), display (32: scanned size of the panel, colour scheme 3 = BWRY, rotation 0,
  transmission mode bit 3 only = direct write, no compression); `00 43` firmware version (`major, minor, length, text, patch`: the layout of the reference firmware, patch = firmware number); `00 44` manufacturer data (16 bytes, layout below);
  `00 50` authenticate (answers status `03`: encryption not enabled); `00 0F` reboot.
* **Direct write**: `00 70` (empty payload; a payload with a size means compression, which is refused with `FF 70`), `00 71 data…` (each chunk acknowledged `00 71`), `00 72` end (optional refresh
  mode and etag bytes are ignored). The frame goes into the OpenDisplay slot (`4D`, default 31) and is drawn; then `00 73` (refreshed) or `00 74` (failed or no slot). Pixel format: scheme 3 BWRY,
  2 bits, black 0, white 1, yellow 2, red 3, leftmost pixel in the top bits: the same as section 2. A frame is `w*h/4` bytes.

What is not there: partial refresh (`00 76`), LED (`00 73` as a command, `00 75`), buzzer, compressed direct write (zlib), AES-CCM encryption (the key flow of the integration is not offered),
`00 41/42` configuration write, `00 45`, DFU `00 51`, the OpenDisplay NFC endpoint `00 83` (the tag has its own NFC commands, section 7), Wi-Fi/LAN, several displays.

Unverified, to be checked against a real client or the reference source (`Firmware-silabs-bg22`, `opendisplay_structs.h`) before anyone relies on it:
(v20 follows the reference layout: manufacturer record = company id, 11 bytes `FF`, temperature `(T + 40) * 2`, battery in 10 mV units with bit 8 in the status byte, loop counter in its top nibble; `00 43` = `major, minor, length, text, patch`.)
Still unverified: field offsets of the
configuration packets (`ic_type` is filled with the closest entry of the list, the EFR32BG22 has none we know of); whether Home Assistant discovers the tag from
the advertisement alone.
