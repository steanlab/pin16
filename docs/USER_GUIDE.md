# User guide

Russian version: `USER_GUIDE.ru.md`.

## What you need
A Solum e-paper tag (see the list in `README.md`), a phone or computer with Chrome or Edge (Web Bluetooth), the control page `docs/index.html` (published with GitHub Pages or opened from a file).

## Connect
1. Touch the tag with the phone (NFC) or press a button, or power it up. The Bluetooth window is open for 1-2 minutes; the tag flashes blue every 5 s while it waits.
2. On the page press **Connect** and choose `pin16-XXXX`. The tag flashes green while connected.
3. The page and the tag firmware should have the same version (the page warns otherwise).

## Pictures
- Press **+** on a free slot and choose any picture. The page converts it to the panel size and to the four panel colours (black, white, yellow, red). The size comes from the tag (200 x 200 or 160 x 296): the "Panel" line in the Tag card shows what the tag found. Presets, dithering, zoom, drag and rotate are in the converter.
- **Show** draws a slot on the panel (20-25 s; the panel keeps the picture without power). **Slideshow** changes pictures on a timer (settings card).
- A raw frame (`.bin`, exactly one frame) can be uploaded too: 10 000 bytes on the 200 x 200 panel, 11 840 bytes on the 160 x 296 panel.
- Orientation: press **Orientation test** (Settings): the page sends a picture with a big F and a red arrow up into the next free slot; **Show** it. The F must read upright and not mirrored, the arrow must point to the top of the tag. If not, change the orientation setting (flip vertically / horizontally / rotate 180°) and draw again; it applies to every slide, also to the ones already stored. The boot splash and the firmware test pictures (Advanced, Panel; picture 8 has an F and UP too) use the same base orientation, so they must agree with the slides. Tell the result for your tag type, it is how the base orientation of each panel gets confirmed. On the 160 x 296 panel use the converter's rotate option for a landscape tag.
- Erasing several slides: **Select slides** turns the tiles into tick boxes (tap a tile), then **Select all** / **Clear selection** / **Erase selected (N)**; one confirmation. If every slide is selected the page sends a single erase-all command (faster, and the flash is left tidy). Settings, name and mode stay; **Done** leaves the mode. The per-slide **Erase** button still works.
- A progress panel stays at the top of the page during uploads, firmware update and preview reading: percent, bytes, time and an estimate of what is left.

## Touch to allow dangerous changes
Firmware update (OTA), rewriting the NFC link, the Bluetooth name, the operating mode and the window times are accepted only within 60 seconds after someone touched the tag with a phone or pressed a button. If the page says "touch the tag with the phone", touch it and repeat within a minute. Slides and ordinary settings do not need this.

## NFC chip
Advanced, "NFC chip (link, text, Wi-Fi)": **Read** shows what is stored (the chip holds one record; by default the link of this page, which a phone opens when it touches the tag). To change it: touch the tag, choose the record kind, fill it in, press **Write**; the tag reads it back and compares.
- **Link**: up to 120 characters of printable ASCII.
- **Text**: up to 120 bytes (about 60 letters in Cyrillic); the phone shows it.
- **Wi-Fi access**: network name, security (WPA2, WPA/WPA2 or open) and password; a phone that touches the tag offers to join the network. **Anyone who touches the tag can read the password in clear text**: use a guest network. The page asks for confirmation and never shows the stored password.
If the phone no longer opens the page, write the old link back. The chip capacity differs between tag types; the page shows it.

## Update the firmware over Bluetooth
Advanced, "Firmware update": choose `pin16_vN.ota.bin` (from the build artifact), touch the tag first, **Upload to the tag**, then **Install and restart**. Use a fresh cell (the tag refuses below 2.7 V) and keep the phone next to the tag. Slides and settings are kept.

## First install on a factory tag
A factory tag is locked. **Unlocking it erases the factory firmware for good, there is no way to make a copy first**, and after the unlock the tag has no bootloader (the factory bootloader is part of what is erased). Do this only with a tag you are ready to give up as it is. The user data page (panel type and size) survives.
You need: an SWD probe (J-Link or a clone, or a CMSIS-DAP adapter that Simplicity Commander or OpenOCD supports), thin wires to the SWD pads of the tag (SWCLK, SWDIO, GND, 3V3 or the battery), Simplicity Commander.
1. Take the build artifact `SOLUM_BTL_FULL_vN.s37` (bootloader + firmware in one image; `_diag` is the diagnostic variant) from the CI run of this repository.
2. Connect the probe, power the tag (battery in, or 3.3 V from the probe).
3. `commander device unlock --device EFR32BG22C224F512IM40` (answer yes: this is the step that erases the factory firmware).
4. `commander flash SOLUM_BTL_FULL_vN.s37 --device EFR32BG22C224F512IM40`
5. Disconnect the probe, take the battery out for a few seconds, put it back: the panel draws the boot splash (about 25 s).
6. Touch the tag with the phone, open the page, **Connect**. From now on update over Bluetooth with `pin16_vN.ota.bin`.
The bootloader is part of the all-in-one image only; later Bluetooth updates do not touch it. Status of the own bootloader of v19: built by CI, to be confirmed on a tag (boot, slide, firmware update); if your CI log shows an error in the bootloader step, the firmware files are still built (without the `…_FULL_…` images).

## If something does not work
- No panel drawing, the page says "panel not supported": the tag reports a controller that is not known (see `docs/hardware.md`). Build the diagnostic firmware (`docs/DIAG.md`) and send the card.
- Connection fails: touch the tag again and press Connect within the window; "Show all devices" helps if the tag is not listed.
