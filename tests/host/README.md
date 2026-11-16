# Host tests (own code)
Runs the real firmware modules `ext_flash`, `settings`, `lz`, `slot`, `pics`, `draw`, `panel`, `tapgate`, `ota`, `beacon`, `led`, `ndef` on a PC, against
fake hardware: a NOR-flash simulator on the SPI driver interface (detects writes without erase, writes without WEL, reads
while in deep power-down, SPI left open, power cuts in the middle of a write), a virtual-time sleeptimer (same rules as
GSDK 4.4: starting a running timer is an error), counters for atomic sections and the EM1 requirement, and a model of the
LED PWM outputs. AddressSanitizer + UndefinedBehaviorSanitizer + LeakSanitizer watch every run.

```
cd tests/host && ./build.sh && ./build/t_all all      # about 20 s; or one of: lz set slot show ota beacon panel
```
Needs gcc with libasan and libubsan. Not part of the firmware build (`build_all.sh` ignores this folder).

What is covered (67 696 checks in v19)
- lz: valid LZ4 streams (5 picture kinds, formats 1 and 2) decode exactly; 300 000 damaged / random / truncated streams, edges of the flash.
- set: round trip of random settings, legacy `S1` record, 300 000 saves (wear: 128 records per sector, both sectors erased evenly),
  40 000 power cuts at every flash operation (never a record that is neither the old nor the new one), noise in the sectors, no flash chip, flash stuck busy.
- slot: garbage in the slide area, 6 000 random uploads / aborts / link closes / erases / verifies, flash closed after every command, OTA area never touched.
- show: slideshow timer for one virtual year at several intervals (no failed timer start, right pace).
- ota: complete update, every refusal (size, battery, CRC, foreign product, damaged GBL, sequence, incomplete, install without verify), 72 aborts at different positions; slides and settings stay untouched; flash closed, EM1 balanced, LED stopped after each.
- beacon: every preset until its cap, also across the 32-bit millisecond wrap (49.7 days), battery guard, hold, event flashes in the middle, 200 000 random operations.

- panel (`test_panel.c`): panel selection from the factory data (10 combinations), the full command / wait / BUSY sequences of EL016 and EL022 compared with the golden lists written from the recordings, a failed line becomes white, the temperature probe, release after every draw.
- geometry: 144 slot draws per panel (raw, LZ4, XOR+LZ4 x 4 orientations x 3 colour tables) equal to an independent model, all test pictures, wrong sizes refused, a damaged stream, slot upload sizes and drawing from the main loop on both panels, unsupported controller.
- ndef (T9): golden Wi-Fi / URI / Text records, UTF-8 validity, limits, 200 000 random byte strings and 36 750 builder round trips without overrun (`ndef.c`).
- settings 'S3': S1/S2 records read with orientation reset, S3 round trip (in set).
- tap gate: closed without a tap, 60 s window to the millisecond, renewal, across the 32-bit tick wrap, OTA begin refused / accepted.

`hw.h` is the real header with only the chip include replaced, and the pin table `hw_pins` is cut out of the real `hw.c`, so the flash and LED pins in the test are the ones of the tag. `draw.c` runs for real (as `real_draw_slot`) on the real `panel.c`; `panel_io` is a recorder.

Not covered: `ble_badge.c` and `bthome.c` (tied to the Bluetooth stack), `panel_io.c`, `hw.c`, `nfc.c` (the bus layers), the real SPI/ADC/timer hardware, supply voltage behaviour.
Stack depth on the tag: command `4F 0D` prints `STK free=N of M` (watermark since start-up).
