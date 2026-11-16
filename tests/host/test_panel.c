// Host test: panel driver (panel.c) replayed against the recorded sequences, drawing (draw.c) on both panel geometries, tap gate.
// The real panel.c, draw.c, lz.c, pics.c, slot.c and tapgate.c run here; only the lowest layer (panel_io) is replaced by a recorder.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "panel.h"
#include "panel_io.h"
#include "draw.h"
#include "lz.h"
#include "pics.h"
#include "slot.h"
#include "settings.h"
#include "ext_flash.h"
#include "tapgate.h"
#include "ota.h"
#include "sl_sleeptimer.h"
#include "fake.h"

extern int draw_real, draw_slot_calls, draw_pic_calls;
bool real_draw_slot(uint32_t a, uint32_t l, uint8_t f, uint8_t o, uint8_t lut);
bool real_draw_pic(uint8_t k, uint8_t o);

static int fails = 0, checks = 0;
#define CK(c, m) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, m); } } while (0)
int panel_test_checks(void) { return checks; }
int panel_test_fails(void) { return fails; }

// ------------------------------------------------------------ recording panel_io
#define TR_MAX 6000
static char tr[TR_MAX][64];
static int  ntr = 0;
static int  pend_lines = 0, pend_bytes = 0;      // data lines not yet written to the trace (one trace line per run)
uint8_t     fp_frame[16384];                      // everything that was sent as data, in order
static int  fp_len = 0;
static int  fp_cs_low = 0;
static uint32_t wait_total_ms = 0;
static int  releases = 0, powers = 0;

static void flush(void)
{
  if (pend_lines) {
    snprintf(tr[ntr++], 64, "D %dx%d", pend_lines, pend_bytes / pend_lines);
    pend_lines = pend_bytes = 0;
  }
}
static void add(const char *fmt, ...)
{
  flush();
  va_list ap; va_start(ap, fmt);
  if (ntr < TR_MAX) vsnprintf(tr[ntr++], 64, fmt, ap);
  va_end(ap);
}
static void tr_clear(void) { ntr = 0; pend_lines = pend_bytes = 0; fp_len = 0; wait_total_ms = 0; releases = powers = 0; }

void pio_idle(void)            { add("I"); }
void pio_power_on(void)        { add("A"); powers++; }
void pio_release(void)         { add("Z"); releases++; }
void pio_wait_ms(uint32_t ms)  { add("W%u", ms); wait_total_ms += ms; }
void pio_rst(bool high)        { add("R%d", high ? 1 : 0); }
void pio_cmd(uint8_t op, const uint8_t *d, uint8_t n, bool keep)
{
  char b[64]; int k = snprintf(b, sizeof b, "C %02X", op);
  for (int i = 0; i < n && k < 60; i++) k += snprintf(b + k, sizeof b - (size_t)k, " %02X", d[i]);
  if (keep) snprintf(b + k, sizeof b - (size_t)k, " *");
  add("%s", b);
  fp_cs_low = keep;
}
void pio_data(const uint8_t *d, size_t n)
{
  if (!fp_cs_low) { fails++; checks++; printf("FAIL data sent while chip select is high\n"); }
  if (fp_len + (int)n > (int)sizeof fp_frame) { fails++; checks++; printf("FAIL frame buffer overflow\n"); return; }
  memcpy(fp_frame + fp_len, d, n); fp_len += (int)n;
  if (pend_lines && pend_bytes / pend_lines != (int)n) flush();
  pend_lines++; pend_bytes += (int)n;
}
void pio_read(uint8_t *d, size_t n) { memset(d, 0x5A, n); }
uint32_t pio_wait_busy(uint32_t timeout_ms, uint8_t level) { add("B %u %u", timeout_ms, level); return 0; }

// ------------------------------------------------------------ golden sequences (written from the recordings, not from panel.c)
static const char *G_EL016_HEAD[] = {
  "W20", "A", "W100", "W200", "R0", "W40", "R1", "W200",
  "C 00 0F 09", "C 01 07", "C 06 40 00 00", "C 61 00 C8 00 C8", "C 50 37", "C 30 08", "C E7 1C", "C E9 01", "C FF A5",
  "C EF 03 2C 07 20 07 10 09 21", "C DC 01", "C DD 06", "C DE 46", "C DA 08", "C E8 00", "C FF E3",
  "C 10 *", "D 200x50", NULL };
static const char *G_EL022_HEAD[] = {
  "W20", "A", "W100", "W200", "R0", "W40", "R1", "W200", "W200", "R0", "W40", "R1", "W200", "W10",
  "C 4D 78", "C 00 07 09", "C 01 07", "C 03 10 54 44", "C 06 0F 0A 2F 25 22 2E 21", "C 50 37", "C 60 02 02", "C 61 00 A0 01 28",
  "C E7 1C", "C E3 22", "C B4 D0", "C B5 03", "C E9 01", "C 30 08", "W300",
  "C 10 *", "D 296x40", NULL };
static const char *G_TAIL[] = {
  "W10", "C 04 00", "W100", "B 1000 1", "W10", "C 12 00", "W300", "B 50000 1",
  "W100", "C 02 00", "W100", "B 2000 1", "C 07 A5", "W100", "Z", NULL };

static int expect(const char *const *head, const char *const *tail, const char *what)
{
  int i = 0;
  for (const char *const *g = head; *g; g++, i++) {
    if (i >= ntr || strcmp(tr[i], *g)) { printf("   %s: step %d: expected \"%s\", got \"%s\"\n", what, i, *g, i < ntr ? tr[i] : "(end)"); return 0; }
  }
  for (const char *const *g = tail; *g; g++, i++) {
    if (i >= ntr || strcmp(tr[i], *g)) { printf("   %s: step %d: expected \"%s\", got \"%s\"\n", what, i, *g, i < ntr ? tr[i] : "(end)"); return 0; }
  }
  if (i != ntr) { printf("   %s: %d extra steps after the expected end, first \"%s\"\n", what, ntr - i, tr[i]); return 0; }
  return 1;
}

// ------------------------------------------------------------ panel selection
static void ud(uint8_t ctrl, uint16_t x, uint16_t y, uint8_t tag)
{
  fake_ud.ctrl = ctrl; fake_ud.xres = x; fake_ud.yres = y; fake_ud.tag = tag;
  panel_init();
}
void panel_test_select(int which)     // 0 EL016, 1 EL022, 2 none
{
  if (which == 0) ud(0x1D, 200, 200, 0x73);
  else if (which == 1) ud(0x1E, 296, 160, 0x74);
  else ud(0x20, 200, 200, 0);
}

static uint32_t rs = 777;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

static bool line_fn_pattern(uint16_t y, uint8_t *l)
{
  for (int i = 0; i < panel_line_bytes(); i++) l[i] = (uint8_t)(y + i);
  return true;
}
static int bad_line = -1;
static bool line_fn_bad(uint16_t y, uint8_t *l)
{
  if ((int)y == bad_line) return false;
  return line_fn_pattern(y, l);
}

static void t_select(void)
{
  struct { uint8_t ctrl; uint16_t x, y; bool ok; const char *name; } T[] = {
    { 0x1D, 200, 200, true, "EL016" }, { 0x1C, 200, 200, true, "EL016" }, { 0x37, 200, 200, true, "EL016" },
    { 0x1E, 296, 160, true, "EL022" },
    { 0x1D, 296, 160, false, "?" }, { 0x1E, 200, 200, false, "?" }, { 0x1E, 160, 296, false, "?" }, { 0x20, 200, 200, false, "?" },
    { 0x00, 0, 0, false, "?" }, { 0xFF, 0xFFFF, 0xFFFF, false, "?" },
  };
  for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
    ud(T[i].ctrl, T[i].x, T[i].y, 0);
    char m[96]; snprintf(m, sizeof m, "userdata %02X %ux%u", T[i].ctrl, T[i].x, T[i].y);
    CK(panel_ok() == T[i].ok, m);
    CK(strcmp(panel_name(), T[i].name) == 0, m);
    if (T[i].ok) {
      CK(panel_line_bytes() * 4 == panel_w() && panel_frame_bytes() == (uint32_t)panel_line_bytes() * panel_h(), "sizes agree");
      CK(panel_line_bytes() <= PANEL_MAX_LINE && panel_frame_bytes() <= PANEL_MAX_FRAME, "sizes fit the buffers");
    } else {
      CK(panel_w() == 0 && panel_h() == 0 && panel_frame_bytes() == 0 && !panel_draw(line_fn_pattern), "no panel: nothing is drawn");
      CK(!real_draw_slot(0x3C100, 100, 1, 0, 0xE4) && !real_draw_pic(1, 0), "no panel: draw refused");
    }
  }
  panel_test_select(0);
  CK(panel_w() == 200 && panel_h() == 200 && panel_frame_bytes() == 10000 && !panel_mirror() && !panel_flip_x(), "EL016 geometry");
  panel_test_select(1);
  CK(panel_w() == 160 && panel_h() == 296 && panel_frame_bytes() == 11840 && panel_mirror() && !panel_flip_x(), "EL022 geometry");
}

static void t_sequences(void)
{
  // EL016
  panel_test_select(0);
  tr_clear();
  CK(panel_draw(line_fn_pattern), "EL016 draw ok");
  CK(expect(G_EL016_HEAD, G_TAIL, "EL016"), "EL016 sequence equals the recording");
  CK(fp_len == 10000 && releases == 1 && powers == 1, "EL016 frame size, one power cycle");
  bool pat = true;
  for (int y = 0; y < 200; y++) for (int i = 0; i < 50; i++) if (fp_frame[y * 50 + i] != (uint8_t)(y + i)) pat = false;
  CK(pat, "EL016 lines arrive in order");
  // EL022
  panel_test_select(1);
  tr_clear();
  CK(panel_draw(line_fn_pattern), "EL022 draw ok");
  CK(expect(G_EL022_HEAD, G_TAIL, "EL022"), "EL022 sequence equals the recording");
  CK(fp_len == 11840 && releases == 1 && powers == 1, "EL022 frame size, one power cycle");
  pat = true;
  for (int y = 0; y < 296; y++) for (int i = 0; i < 40; i++) if (fp_frame[y * 40 + i] != (uint8_t)(y + i)) pat = false;
  CK(pat, "EL022 lines arrive in order");
  // a failing line becomes white, the rest is drawn, the result is false, the panel is released
  for (int which = 0; which < 2; which++) {
    panel_test_select(which);
    for (int k = 0; k < 3; k++) {
      bad_line = k == 0 ? 0 : k == 1 ? (int)panel_h() - 1 : (int)(rnd() % panel_h());
      tr_clear();
      CK(!panel_draw(line_fn_bad), "failed line: draw returns false");
      CK(releases == 1 && fp_len == (int)panel_frame_bytes(), "failed line: panel released, whole frame sent");
      bool white = true; for (int i = 0; i < panel_line_bytes(); i++) if (fp_frame[bad_line * panel_line_bytes() + i] != 0x55) white = false;
      CK(white, "failed line is white");
    }
  }
  bad_line = -1;
  // temperature probe: same wake-up and set-up as a draw, then 04 / 41 / 40 read, then power down
  panel_test_select(0);
  tr_clear();
  uint8_t t[3];
  CK(panel_probe_temp(t) && t[0] == 0x5A && releases == 1, "temperature probe");
  CK(strcmp(tr[ntr - 1], "Z") == 0, "probe releases the panel");
  // total time of a draw: waits plus busy limits stay below the watchdog-friendly limits
  panel_test_select(0); tr_clear(); panel_draw(line_fn_pattern);
  CK(wait_total_ms < 2000, "fixed waits of a draw below 2 s (the rest is BUSY)");
}

// ------------------------------------------------------------ geometry: slots and pictures on both panels
static int lz4_enc2(const uint8_t *s, int n, uint8_t *d)       // minimal LZ4 block encoder, same format as the app
{
  static int ht[4096];
  for (int i = 0; i < 4096; i++) ht[i] = -1;
  int ip = 0, an = 0, op = 0;
  while (ip + 4 <= n) {
    uint32_t v = (uint32_t)s[ip] | (uint32_t)s[ip + 1] << 8 | (uint32_t)s[ip + 2] << 16 | (uint32_t)s[ip + 3] << 24;
    uint32_t h = (v * 2654435761u) >> 20;
    int c = ht[h]; ht[h] = ip;
    if (c >= 0 && ip - c <= 2048 && memcmp(s + c, s + ip, 4) == 0) {
      int m = 4; while (ip + m < n && s[c + m] == s[ip + m]) m++;
      int ll = ip - an, ml = m - 4;
      d[op++] = (uint8_t)((ll >= 15 ? 15 : ll) << 4 | (ml >= 15 ? 15 : ml));
      if (ll >= 15) { int r = ll - 15; while (r >= 255) { d[op++] = 255; r -= 255; } d[op++] = (uint8_t)r; }
      memcpy(d + op, s + an, (size_t)ll); op += ll;
      int off = ip - c; d[op++] = (uint8_t)off; d[op++] = (uint8_t)(off >> 8);
      if (ml >= 15) { int r = ml - 15; while (r >= 255) { d[op++] = 255; r -= 255; } d[op++] = (uint8_t)r; }
      ip += m; an = ip;
    } else ip++;
  }
  if (an < n) {
    int ll = n - an;
    d[op++] = (uint8_t)((ll >= 15 ? 15 : ll) << 4);
    if (ll >= 15) { int r = ll - 15; while (r >= 255) { d[op++] = 255; r -= 255; } d[op++] = (uint8_t)r; }
    memcpy(d + op, s + an, (size_t)ll); op += ll;
  }
  return op;
}

static void make(uint8_t *f, int kind)
{
  int w = panel_w(), h = panel_h(), lb = panel_line_bytes();
  for (int y = 0; y < h; y++) for (int x = 0; x < lb; x++) {
    uint8_t b;
    switch (kind) {
      case 0: b = (uint8_t)rnd(); break;
      case 1: b = ((y / 16 + x / 4) & 1) ? 0xAA : 0x00; break;
      case 2: b = (uint8_t)(x * 7 + y * 3); break;
      default: b = (rnd() % 9 == 0) ? (uint8_t)rnd() : 0x55; break;
    }
    f[y * lb + x] = b;
  }
  (void)w;
}

// What the panel must receive for scan line y: written from the rules in panel.h / draw.h, not from draw.c.
static void model(const uint8_t *frame, int orient, uint8_t lut, bool raw, uint8_t *out)
{
  int w = panel_w(), h = panel_h(), lb = panel_line_bytes();
  for (int y = 0; y < h; y++) {
    // v20 rules: EL016 is stored as is, EL022 reverses the lines (nothing else); orient bit 0 / 1 reverse lines / pixels again (all formats)
    bool names_el016 = strcmp(panel_name(), "EL016") == 0;
    bool rev_l = names_el016 ? (orient & 1) != 0 : !(orient & 1);
    bool rev_p = (orient & 2) != 0;
    int yy = rev_l ? h - 1 - y : y;
    (void)raw;
    for (int x = 0; x < lb; x++) out[y * lb + x] = 0;
    for (int x = 0; x < w; x++) {
      int px = rev_p ? w - 1 - x : x;
      int code = (frame[yy * lb + px / 4] >> (6 - 2 * (px % 4))) & 3;
      int col = (lut >> (2 * code)) & 3;
      out[y * lb + x / 4] |= (uint8_t)(col << (6 - 2 * (x % 4)));
    }
  }
}

static void t_geometry(void)
{
  static uint8_t fr[12000], st[13000], want[12000], xr[12000];
  const uint32_t A = 0x3C100;
  uint8_t luts[3] = { 0xE4, 0x1B, 0xB1 };
  for (int which = 0; which < 2; which++) {
    panel_test_select(which);
    int lb = panel_line_bytes(), h = panel_h(); uint32_t fb = panel_frame_bytes();
    int runs = 0, ok_runs = 0;
    for (int kind = 0; kind < 4; kind++) {
      make(fr, kind);
      for (int fmt = 0; fmt < 3; fmt++) {
        uint32_t n;
        if (fmt == 0) { memcpy(st, fr, fb); n = fb; }
        else {
          const uint8_t *src = fr;
          if (fmt == 2) { for (int y = 0; y < h; y++) for (int x = 0; x < lb; x++) xr[y * lb + x] = (uint8_t)(fr[y * lb + x] ^ (y ? fr[(y - 1) * lb + x] : 0)); src = xr; }
          n = (uint32_t)lz4_enc2(src, (int)fb, st);
        }
        fl_reset();
        for (uint32_t i = 0; i < n; i++) fl[A + i] = st[i];
        if (fmt) {
          uint8_t id[3]; CK(xf_begin(id), "flash opens");
          CK(lz_check(A, n, fmt == 2, (uint16_t)lb, (uint16_t)h) == 1, "lz_check accepts the stream");
          CK(lz_check(A, n, fmt == 2, (uint16_t)lb, (uint16_t)(h + 1)) == 0 && lz_check(A, n, fmt == 2, (uint16_t)lb, (uint16_t)(h - 1)) == 0, "lz_check with a wrong line count refuses");
          xf_end();
        }
        for (int orient = 0; orient < 4; orient++) for (int l = 0; l < 3; l++) {
          tr_clear();
          bool ok = real_draw_slot(A, n, (uint8_t)fmt, (uint8_t)orient, luts[l]);
          model(fr, orient, luts[l], fmt == 0, want);
          runs++;
          bool same = ok && fp_len == (int)fb && memcmp(fp_frame, want, fb) == 0;
          if (same) ok_runs++;
          else { printf("   %s kind %d fmt %d orient %d lut %02X: ok=%d len=%d\n", panel_name(), kind, fmt, orient, luts[l], ok, fp_len); }
          if (!same && fails > 20) return;
          CK(spi_open_count == 0 && fl_powered_down, "flash closed after a draw");
        }
      }
    }
    CK(ok_runs == runs, "every slot draw gives the frame the model expects");
    printf("   %s: %d slot draws (raw, LZ4, XOR-LZ4 x 4 orientations x 3 colour tables) all equal to the model\n", panel_name(), runs);

    // wrong sizes
    fl_reset();
    CK(!real_draw_slot(A, fb - 1, 0, 0, 0xE4) && !real_draw_slot(A, fb + 1, 0, 0, 0xE4), "raw slot of the wrong size is refused");
    CK(!real_draw_slot(A, (which == 0 ? 11840u : 10000u), 0, 0, 0xE4), "raw slot of the other panel's size is refused");
    // damaged LZ slot: false, but the draw runs to the end (white lines) and the panel is released
    make(fr, 0);
    uint32_t n = (uint32_t)lz4_enc2(fr, (int)fb, st);
    for (uint32_t i = 0; i < n; i++) fl[A + i] = st[i];
    tr_clear();
    bool r = real_draw_slot(A, n - 7, 1, 0, 0xE4);           // truncated stream: cannot decode to the end
    CK(!r, "truncated stream: draw returns false");
    CK(releases == 1 && spi_open_count == 0 && fp_len == (int)fb, "truncated stream: whole frame sent (white lines), panel and flash released");
    fl[A + n / 2] ^= 0xFF; fl[A + n / 3] ^= 0x5A;           // flipped bytes: the stream may still decode (then the picture is wrong, no crash)
    tr_clear();
    (void)real_draw_slot(A, n, 1, 0, 0xE4);
    CK(releases == 1 && spi_open_count == 0, "flipped bytes: panel and flash released");

    // pictures
    int pics = 0;
    for (uint8_t k = 0; k < 16; k++) {
      if (!pic_valid(k)) continue;
      for (int orient = 0; orient < 4; orient++) {
        tr_clear();
        CK(real_draw_pic(k, (uint8_t)orient), "picture drawn");
        CK(fp_len == (int)fb, "picture: whole frame sent");
        // compare with the picture generator through the same rules as a raw slot
        for (int y = 0; y < h; y++) pic_line(k, (uint16_t)y, fr + y * lb, panel_w(), (uint16_t)h);
        model(fr, orient, 0xE4, true, want);
        CK(memcmp(fp_frame, want, fb) == 0, "picture equals the model");
        pics++;
      }
    }
    CK(pics > 0, "there are pictures");
    printf("   %s: %d picture draws checked\n", panel_name(), pics);
  }
  panel_test_select(0);
}

// ------------------------------------------------------------ slots through slot.c on both panels (sizes, draw from the main loop)
static void sc(uint8_t c, const uint8_t *d, int n)
{
  uint8_t b[260]; b[0] = c; if (n) memcpy(b + 1, d, (size_t)n);
  slot_cmd(b, (uint8_t)(n + 1));
}
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t crc32(const uint8_t *p, uint32_t n)
{
  uint32_t c = 0xFFFFFFFFu;
  while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
  return ~c;
}
static bool say_has(const char *s) { return strstr(say_last, s) != NULL; }
static bool upload(int slot, const uint8_t *st, uint32_t n, int fmt)
{
  uint8_t b[16]; b[0] = (uint8_t)slot; put32(b + 1, n); put32(b + 5, crc32(st, n)); b[9] = 8; b[10] = (uint8_t)fmt;
  sc(0x42, b, 11);
  if (!say_has("UP READY")) return false;
  uint32_t off = 0; uint16_t seq = 0;
  while (off < n) {
    uint32_t k = n - off > 200 ? 200 : n - off;
    uint8_t d[210]; d[0] = (uint8_t)seq; d[1] = (uint8_t)(seq >> 8); memcpy(d + 2, st + off, k);
    sc(0x43, d, (int)k + 2);
    if (say_has("FAIL")) return false;
    off += k; seq++;
  }
  sc(0x44, NULL, 0);
  return say_has("UP OK");
}

static void t_slot_sizes(void)
{
  static uint8_t fr[12000], st[13000], want[12000];
  for (int which = 0; which < 3; which++) {
    panel_test_select(which);
    fl_reset(); settings_load();
    uint32_t fb = panel_frame_bytes();
    if (which == 2) {
      memset(st, 0x55, 10000);
      CK(!upload(0, st, 10000, 0) && say_has("panel"), "no supported panel: upload refused with 'panel'");
      continue;
    }
    make(fr, 2);
    CK(upload(0, fr, fb, 0), "raw slot of the panel size accepted");
    CK(!upload(1, fr, fb - 1, 0) && say_has("len"), "raw slot one byte short refused");
    CK(!upload(1, fr, fb + 1, 0) && say_has("len"), "raw slot one byte long refused");
    CK(!upload(1, fr, which == 0 ? 11840u : 10000u, 0), "raw slot of the other panel refused");
    uint32_t n = (uint32_t)lz4_enc2(fr, (int)fb, st);
    CK(upload(2, st, n, 1), "LZ4 slot accepted");
    // a stream of the other geometry has the right bytes but the wrong number of lines
    static uint8_t other[12000], os[13000];
    memset(other, 0x55, sizeof other);
    uint32_t on = (uint32_t)lz4_enc2(other, (int)(which == 0 ? 11840 : 10000), os);
    CK(!upload(3, os, on, 1) && say_has("decode"), "LZ4 stream of the other panel's frame refused ('decode')");
    // draw slot 0 and 2 from the main loop, as the slideshow and the button do
    draw_real = 1;
    for (int s = 0; s < 3; s += 2) {
      tr_clear();
      slot_request_draw(s, false);
      for (int i = 0; i < 10 && slot_draw_pending(); i++) slot_poll();
      CK(!slot_draw_pending(), "draw request served");
      model(fr, g_set.orient, g_set.lut, s == 0, want);
      CK(fp_len == (int)fb && memcmp(fp_frame, want, fb) == 0, "slot drawn from the main loop equals the model");
    }
    draw_real = 0;
    CK(spi_open_count == 0 && fl_powered_down, "flash closed");
  }
  panel_test_select(0);
}

// ------------------------------------------------------------ tap gate
static void ob_raw(uint32_t len)
{
  uint8_t b[12] = { 0x46, 1 }; put32(b + 2, len); put32(b + 6, 0); b[10] = 8; ota_cmd(b, 11);
}
static void t_gate(void)
{
  vt_advance_ms(3600000);                        // an hour: whatever earlier tests touched is long closed
  CK(!tapgate_open() && tapgate_left_s() == 0, "gate closed without a tap");
  ob_raw(1000);
  CK(strstr(say_last, "tap") != NULL, "OTA begin refused without a tap");
  CK(!ota_busy(), "refused OTA is not running");
  tapgate_touch();
  CK(tapgate_open() && tapgate_left_s() == 60, "open after a tap, 60 s left");
  vt_advance_ms(30000);
  CK(tapgate_open() && tapgate_left_s() == 30, "30 s later 30 s left");
  vt_advance_ms(29000);
  CK(tapgate_open() && tapgate_left_s() == 1, "59 s later 1 s left");
  vt_advance_ms(999);
  CK(tapgate_open(), "59.999 s: still open");
  vt_advance_ms(2);
  CK(!tapgate_open() && tapgate_left_s() == 0, "60 s: closed");
  tapgate_touch(); vt_advance_ms(10000); tapgate_touch(); vt_advance_ms(55000);
  CK(tapgate_open(), "a second tap renews the time");
  // across the 32-bit tick wrap (36.4 hours at 32768 Hz)
  vt_set_ms(36ULL * 3600 * 1000 + 24 * 60 * 1000 - 20000);
  tapgate_touch();
  vt_advance_ms(15000);
  CK(tapgate_open(), "open shortly before the 32-bit tick counter wraps");
  vt_advance_ms(10000);
  CK(tapgate_open(), "open across the wrap");
  vt_advance_ms(40000);
  CK(!tapgate_open(), "closed 65 s after the tap, across the wrap");
  // OTA begin works again after a tap
  tapgate_touch();
  ob_raw(1000);
  CK(strstr(say_last, "tap") == NULL, "OTA begin passes the gate after a tap");
  uint8_t ab[2] = { 0x46, 5 }; ota_cmd(ab, 2);
  CK(!ota_busy(), "OTA aborted");
}

void t_panel_all(void)
{
  printf("T6 panel: selection, recorded sequences\n");
  t_select();
  t_sequences();
  printf("T7 geometry on both panels\n");
  t_geometry();
  t_slot_sizes();
  printf("T8 tap gate\n");
  t_gate();
}
