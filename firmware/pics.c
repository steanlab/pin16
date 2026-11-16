// pin16: pictures made in the firmware, see pics.h. Own code.
#include <stdio.h>
#include <string.h>
#include "pics.h"
#include "fwver.h"

extern const uint8_t bg_font7x14[95][14];

static void put(uint8_t *l, unsigned x, uint8_t code)
{
  unsigned sh = 6 - 2 * (x & 3);
  l[x >> 2] = (uint8_t)((l[x >> 2] & ~(3u << sh)) | ((unsigned)code << sh));
}

static void fill(uint8_t *l, uint8_t code, unsigned w) { memset(l, code * 0x55, w / 4); }

// One row of a text of scaled glyphs: y inside [y0, y0 + 14 * scale)
static void text_row(uint8_t *l, unsigned w, const char *s, unsigned x0, unsigned y0, unsigned scale, unsigned y, uint8_t code)
{
  if (y < y0 || y >= y0 + 14 * scale) return;
  unsigned row = (y - y0) / scale;
  unsigned x = x0;
  for (; *s; s++) {
    unsigned c = (unsigned char)*s;
    if (c < 32 || c > 126) c = '?';
    uint8_t bits = bg_font7x14[c - 32][row];
    for (unsigned b = 0; b < 7; b++) {
      if (bits & (0x80u >> b)) {
        for (unsigned k = 0; k < scale; k++) {
          if (x + b * scale + k < w) put(l, x + b * scale + k, code);
        }
      }
    }
    x += 7 * scale;
  }
}

bool pic_valid(uint8_t kind) { return kind >= 1 && kind <= PIC_LAST; }

void pic_line(uint8_t kind, uint16_t y, uint8_t *l, uint16_t w, uint16_t h)
{
  switch (kind) {
    case PIC_SPLASH: {
      static char ver[32];
      if (ver[0] == 0) snprintf(ver, sizeof(ver), "built by steanlab, ver %u", (unsigned)BG_FW_VERSION);
      bool narrow = (strlen(ver) * 7 + 4 > w);                             // 175 pixels do not fit 160: two lines then
      unsigned s1 = (w >= 189) ? 5 : 4;                                    // "pin16": 5 characters of 7 pixels, 175 pixels on 200, 140 on 160
      fill(l, 0, w);                                                       // black
      text_row(l, w, "pin16", (w - 35 * s1) / 2, (h - 14 * s1) / 2 - 8, s1, y, 1);
      if (!narrow) {
        text_row(l, w, ver, 2, h - 16, 1, y, 1);                           // bottom left
      } else {
        const char *sp = strstr(ver, ", ");                                // "built by steanlab" / "ver N"
        char first[24];
        size_t n = sp ? (size_t)(sp - ver) : strlen(ver);
        if (n >= sizeof(first)) n = sizeof(first) - 1;
        memcpy(first, ver, n);
        first[n] = 0;
        text_row(l, w, first, 2, h - 32, 1, y, 1);
        text_row(l, w, sp ? sp + 2 : "", 2, h - 16, 1, y, 1);
      }
      break;
    }
    case PIC_BARS:
      if (y < h / 2) {
        for (unsigned x = 0; x < w; x++) put(l, x, (uint8_t)(x / (w / 4)));              // black, white, yellow, red
      } else {
        unsigned band = (unsigned)(y - h / 2) / ((h / 2) / 4);
        fill(l, (uint8_t)(3 - (band > 3 ? 3 : band)), w);                                // red, yellow, white, black
      }
      break;
    case PIC_WHITE:  fill(l, 1, w); break;
    case PIC_BLACK:  fill(l, 0, w); break;
    case PIC_YELLOW: fill(l, 2, w); break;
    case PIC_RED:    fill(l, 3, w); break;
    case PIC_CHECK:
      for (unsigned x = 0; x < w; x++) put(l, x, (uint8_t)(((x + y) & 1) ? 1 : 0));
      break;
    case PIC_FRAME:
      fill(l, 1, w);
      if (y < 2 || y >= h - 2) {
        fill(l, 0, w);
      } else {
        put(l, 0, 0); put(l, 1, 0); put(l, w - 2, 0); put(l, w - 1, 0);
        if (y >= h / 2 - 20 && y < h / 2 + 20) for (unsigned x = w / 2 - 20; x < w / 2 + 20; x++) put(l, x, 2);   // centre square, yellow
        if (y >= 6 && y < 18) for (unsigned x = 6; x < 18; x++) put(l, x, 0);                                   // corner mark, top left
        if (y < 10) for (unsigned x = 20; x < w; x += 20) { put(l, x, 0); put(l, x + 1, 0); }                    // ticks along the top edge
        text_row(l, w, "F", 24, 30, 4, y, 0);                                                                   // letter F, top left: must read upright and not mirrored
        text_row(l, w, "UP", w / 2 - 14, 14, 2, y, 0);                                                          // marks the top edge
        unsigned d = (unsigned)y * (w - 1) / (h - 1);                                                           // diagonals, red
        put(l, d, 3);
        put(l, w - 1 - d, 3);
      }
      break;
    default:
      fill(l, 1, w);
      break;
  }
}
