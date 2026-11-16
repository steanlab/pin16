// pin16: streaming LZ4 block decoder, see lz.h. Own code.
#include "lz.h"
#include "ext_flash.h"

static uint8_t  ring[LZ_WIN];
static uint8_t  prev[LZ_MAXLINE];
static uint16_t d_lb;
static uint8_t  cache[64];
static uint32_t d_addr, d_frem, d_pos;
static uint16_t d_lit, d_ml, d_off;
static uint8_t  d_ci, d_cn, d_ph, d_err, d_xor;

static uint32_t in_left(void) { return d_frem + (uint32_t)(d_cn - d_ci); }

static int rd(void)
{
  if (d_ci == d_cn) {
    uint32_t n = d_frem < sizeof(cache) ? d_frem : sizeof(cache);
    if (!n) { d_err = 1; return 0; }
    xf_read(d_addr, cache, n);
    d_addr += n; d_frem -= n; d_ci = 0; d_cn = (uint8_t)n;
  }
  return cache[d_ci++];
}

static int emit(uint8_t b) { ring[d_pos & (LZ_WIN - 1)] = b; d_pos++; return b; }

static int dec_get(void)
{
  for (;;) {
    int x;
    if (d_err) return -1;
    switch (d_ph) {
      case 0:
        if (!in_left()) return -1;
        x = rd(); d_lit = (uint16_t)(x >> 4); d_ml = (uint16_t)(x & 15);
        if (d_lit == 15) { do { x = rd(); d_lit += (uint16_t)x; } while (x == 255 && !d_err); }
        d_ph = 1;
        break;
      case 1:
        if (d_lit) { d_lit--; x = rd(); if (d_err) return -1; return emit((uint8_t)x); }
        d_ph = 2;
        break;
      case 2:
        if (!in_left()) { d_ph = 0; return -1; }      // the last sequence has literals only
        x = rd(); d_off = (uint16_t)(x | (rd() << 8));
        if (d_off == 0 || d_off > LZ_WIN || d_off > d_pos) { d_err = 1; return -1; }
        if (d_ml == 15) { do { x = rd(); d_ml += (uint16_t)x; } while (x == 255 && !d_err); }
        d_ml += 4;
        d_ph = 3;
        break;
      case 3:
        if (d_ml) { d_ml--; return emit(ring[(d_pos - d_off) & (LZ_WIN - 1)]); }
        d_ph = 0;
        break;
    }
  }
}

int lz_begin(uint32_t addr, uint32_t len, int xor_lines, uint16_t line_bytes)
{
  if (line_bytes == 0 || line_bytes > LZ_MAXLINE) return 0;
  d_lb = line_bytes;
  d_addr = addr; d_frem = len; d_pos = 0; d_lit = 0; d_ml = 0; d_off = 0; d_ci = 0; d_cn = 0; d_ph = 0; d_err = 0;
  d_xor = xor_lines ? 1 : 0;
  for (int i = 0; i < LZ_MAXLINE; i++) prev[i] = 0;
  return len != 0;
}

int lz_line(uint8_t *out)
{
  for (int i = 0; i < d_lb; i++) {
    int b = dec_get();
    if (b < 0) return 0;
    if (d_xor) { b ^= prev[i]; prev[i] = (uint8_t)b; }
    out[i] = (uint8_t)b;
  }
  return 1;
}

int lz_check(uint32_t addr, uint32_t len, int xor_lines, uint16_t line_bytes, uint16_t lines)
{
  uint8_t line[LZ_MAXLINE];
  if (!lz_begin(addr, len, xor_lines, line_bytes)) return 0;
  for (int y = 0; y < lines; y++) if (!lz_line(line)) return 0;
  return !d_err && d_lit == 0 && d_ml == 0 && in_left() == 0;
}
