// pin16: drawing a slot or a test picture on the panel. Own code.
#ifndef DRAW_H
#define DRAW_H

#include <stdbool.h>
#include <stdint.h>

// Draws a stored stream (format 0 raw, 1 LZ4, 2 line-XORed LZ4). The external flash is opened here.
// orient (user setting, on top of what the panel profile already does): bit 0 lines reversed (all formats), bit 1 pixels reversed. lut: colour table, code i -> (lut >> 2i) & 3, default 0xE4.
// false: no supported panel, no flash, or damaged data (white lines were drawn for the damaged ones).
bool draw_slot(uint32_t addr, uint32_t len, uint8_t fmt, uint8_t orient, uint8_t lut);
bool draw_pic(uint8_t kind, uint8_t orient);       // a picture made by the firmware (pics.h), the flash is not used
bool draw_busy(void);                              // a refresh is running
bool draw_probe_temp(uint8_t out[3]);

#endif
