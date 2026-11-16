// pin16: drawing a slot or a test picture, see draw.h. Own code.
#include <string.h>
#include "draw.h"
#include "panel.h"
#include "ext_flash.h"
#include "lz.h"
#include "pics.h"
#include "sysinfo.h"

static bool     busy = false;
static uint8_t  src_fmt;                 // 0 raw, 1 LZ4, 2 line-XORed LZ4
static uint32_t src_addr, src_len;
static uint8_t  src_pic;                 // not 0: lines come from pics.c
static uint8_t  orient, lut;
static uint8_t  cur[PANEL_MAX_LINE];     // the decoded source line
static int      cur_y;
static bool     failed;

// Source line yy (in the order of the stored frame). Lines of LZ4 slots can only be decoded in order: a jump back starts again.
static bool src_line(uint16_t yy)
{
  if ((int)yy == cur_y) {
    return true;
  }
  if (yy >= panel_h()) {
    return false;
  }
  uint16_t lb = panel_line_bytes();
  if (src_pic) {
    pic_line(src_pic, yy, cur, panel_w(), panel_h());
    cur_y = yy;
    return true;
  }
  if (src_fmt == 0) {
    xf_read(src_addr + (uint32_t)yy * lb, cur, lb);
  } else {
    if ((int)yy != cur_y + 1) {
      if (!lz_begin(src_addr, src_len, src_fmt == 2, lb)) {
        return false;
      }
      for (int k = 0; k < (int)yy; k++) {
        if (!lz_line(cur)) {
          return false;
        }
      }
    }
    if (!lz_line(cur)) {
      return false;
    }
  }
  cur_y = yy;
  return true;
}

// Scan line y of the panel: the slide line (mirrored by the panel profile, optionally reversed by orient bit 0), the pixels
// (reversed by the panel profile, optionally again by orient bit 1) and the colour table. Slides and the pictures made in
// the firmware (splash, test pictures) all go through here, so they always agree.
static bool scan_line(uint16_t y, uint8_t *out)
{
  uint16_t h = panel_h(), w = panel_w();
  bool rev_lines = panel_mirror() != ((orient & 1) != 0);
  bool rev_px = panel_flip_x() != ((orient & 2) != 0);
  uint16_t yy = rev_lines ? (uint16_t)(h - 1 - y) : y;
  if (!src_line(yy)) {
    failed = true;
    return false;
  }
  memset(out, 0, panel_line_bytes());
  for (uint16_t x = 0; x < w; x++) {
    uint16_t px = rev_px ? (uint16_t)(w - 1 - x) : x;
    uint8_t code = (cur[px >> 2] >> (6 - 2 * (px & 3))) & 3;
    uint8_t col = (uint8_t)((lut >> (2 * code)) & 3);
    out[x >> 2] |= (uint8_t)(col << (6 - 2 * (x & 3)));
  }
  return true;
}

static bool run(void)
{
  busy = true;
  failed = false;
  cur_y = -2;
  sysinfo_load_begin();
  bool ok = panel_draw(scan_line);
  sysinfo_load_end();
  busy = false;
  if (ok && !failed) {
    sysinfo_draw_done();
    return true;
  }
  return false;
}

bool draw_slot(uint32_t addr, uint32_t len, uint8_t fmt, uint8_t o, uint8_t l)
{
  uint8_t id[3];
  if (!panel_ok() || (fmt == 0 && len != panel_frame_bytes()) || !xf_begin(id)) {
    return false;
  }
  orient = o; lut = l;
  src_fmt = fmt; src_addr = addr; src_len = len; src_pic = 0;
  bool ok = run();
  xf_end();
  return ok;
}

bool draw_pic(uint8_t kind, uint8_t o)
{
  if (!panel_ok() || !pic_valid(kind)) {
    return false;
  }
  orient = o; lut = 0xE4;
  src_fmt = 0; src_addr = 0; src_len = 0; src_pic = kind;
  return run();
}

bool draw_busy(void) { return busy; }

bool draw_probe_temp(uint8_t out[3])
{
  if (busy) {
    return false;                                       // a draw is running
  }
  busy = true;
  bool ok = panel_probe_temp(out);
  busy = false;
  return ok;
}
