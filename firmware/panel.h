// pin16: the e-paper panel (JD-type controller, black / white / yellow / red, 2 bits per pixel). Own code.
// Two panels are supported, both recorded from real tags with the hardware card (docs/hardware.md):
//   EL016 (userdata ctrl type 0x1D): 200 pixels per line, 200 lines, 50 bytes per line, 10 000 bytes per frame
//   EL022 (userdata ctrl type 0x1E): 160 pixels per line, 296 lines, 40 bytes per line, 11 840 bytes per frame
// A tag with another controller type is not drawn on: the Bluetooth part still works and the card shows what is there.
#ifndef PANEL_H
#define PANEL_H

#include <stdbool.h>
#include <stdint.h>

#define PANEL_MAX_LINE   64          // bytes per line the code has room for (256 pixels)
#define PANEL_MAX_FRAME  12000UL     // a frame has to fit into 3 flash sectors together with the slot header

// Pixel codes in a line, as in the slots: 0 black, 1 white, 2 yellow, 3 red; the first pixel is in the top bits of the first byte.
typedef bool (*panel_line_fn)(uint16_t y, uint8_t *line);   // y = scan line 0..h-1 in the order they are sent; false = no data (white is drawn)

bool     panel_init(void);                  // reads the factory data, picks the profile. true: this panel can be drawn on
bool     panel_ok(void);
uint16_t panel_w(void);                     // pixels per scan line
uint16_t panel_h(void);                     // scan lines per frame
uint16_t panel_line_bytes(void);            // panel_w() / 4
uint32_t panel_frame_bytes(void);           // panel_line_bytes() * panel_h()
bool     panel_mirror(void);                // slide line 0 is the last scan line (both panels)
bool     panel_flip_x(void);                // slide pixel 0 is the last pixel of a scan line (EL016 only)
const char *panel_name(void);               // "EL016", "EL022", or "?" if not supported
bool     panel_draw(panel_line_fn fn);      // a whole refresh (about 25 s, the chip sleeps while it waits); false if fn failed on a line
bool     panel_probe_temp(uint8_t out[3]);  // experimental: read the temperature sensor of the panel

#endif
