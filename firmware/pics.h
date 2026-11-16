// pin16: pictures made in the firmware (boot splash, test patterns). Lines are produced on demand, nothing is stored.
// Own code.
#ifndef PICS_H
#define PICS_H
#include <stdbool.h>
#include <stdint.h>

#define PIC_SPLASH   1    // "pin16 / built by steanlab, ver N", white on black
#define PIC_BARS     2    // colour bars: four vertical (top) and four horizontal (bottom) bands
#define PIC_WHITE    3
#define PIC_BLACK    4
#define PIC_YELLOW   5
#define PIC_RED      6
#define PIC_CHECK    7    // pixel checker board (white / black)
#define PIC_FRAME    8    // frame, diagonals, centre square and a corner mark: shows orientation and the whole area
#define PIC_LAST     8

bool pic_valid(uint8_t kind);
// One line of w pixels, 2 bits per pixel (w / 4 bytes), codes as in the slots: black 0, white 1, yellow 2, red 3. y: 0..h-1.
void pic_line(uint8_t kind, uint16_t y, uint8_t *line, uint16_t w, uint16_t h);
#endif
