// Host test of the pin16 firmware modules with simulated flash, virtual time and ASan/UBSan.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ext_flash.h"
#include "settings.h"
#include "lz.h"
#include "slot.h"
#include "ota.h"
#include "beacon.h"
#include "led.h"
#include "tapgate.h"
#include "panel.h"
#include "draw.h"
#include "ndef.h"
#include "od.h"
#include "sl_sleeptimer.h"
#include "fake.h"
#include "em_gpio.h"
#include "em_timer.h"
#include "em_cmu.h"
#include "em_core.h"

static int fails = 0, checks = 0;
#define CK(c, m) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, m); } } while (0)
static uint32_t rs = 12345;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t crc32(const uint8_t *p, uint32_t n)
{
  uint32_t c = 0xFFFFFFFFu;
  while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
  return ~c;
}
static bool flash_idle(void) { return spi_open_count == 0 && fl_powered_down; }

// ---------------------------------------------------------------- LZ4 encoder (test side)
static int lz4_enc(const uint8_t *s, int n, uint8_t *d)
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
static void make_frame(int kind, uint8_t *f)
{
  for (int y = 0; y < 200; y++) for (int x = 0; x < 50; x++) {
    uint8_t b = 0;
    switch (kind) {
      case 0: b = 0x55; break;                                              // flat
      case 1: b = (uint8_t)(rnd()); break;                                  // noise
      case 2: b = (uint8_t)((x * 5 + (y / 8) * 17) & 0xFF); break;           // gradient
      case 3: b = ((y / 20 + x / 10) & 1) ? 0xAA : 0x00; break;               // blocks
      default: b = (rnd() % 8 == 0) ? (uint8_t)rnd() : 0x00; break;          // sparse
    }
    f[y * 50 + x] = b;
  }
}

// ---------------------------------------------------------------- T1: LZ decoder
static void t_lz(void)
{
  printf("T1 lz\n");
  fl_reset();
  uint8_t id[3]; xf_begin(id);
  uint8_t fr[10000], xf[10000], st[12000];
  for (int kind = 0; kind < 5; kind++) {
    make_frame(kind, fr);
    for (int fmt = 1; fmt <= 2; fmt++) {
      const uint8_t *src = fr;
      if (fmt == 2) { for (int y = 0; y < 200; y++) for (int x = 0; x < 50; x++) xf[y * 50 + x] = (uint8_t)(fr[y * 50 + x] ^ (y ? fr[(y - 1) * 50 + x] : 0)); src = xf; }
      int n = lz4_enc(src, 10000, st);
      memcpy(fl + 0x3C100, st, (size_t)n);
      CK(lz_check(0x3C100, (uint32_t)n, fmt == 2, 50, 200) == 1, "valid stream passes lz_check");
      lz_begin(0x3C100, (uint32_t)n, fmt == 2, 50);
      uint8_t ln[50]; bool same = true;
      for (int y = 0; y < 200; y++) { if (!lz_line(ln)) { same = false; break; } if (memcmp(ln, fr + y * 50, 50)) same = false; }
      CK(same, "decoded frame equals the original");
      CK(lz_check(0x3C100, (uint32_t)n - 1, fmt == 2, 50, 200) == 0, "truncated stream refused");
      CK(lz_check(0x3C100, (uint32_t)n + 7, fmt == 2, 50, 200) == 0, "stream with extra bytes refused");
    }
  }
  // fuzz: mutated valid streams and pure noise; ASan/UBSan watch the decoder
  make_frame(3, fr);
  int n = lz4_enc(fr, 10000, st);
  long ok = 0, bad = 0;
  for (int it = 0; it < 300000; it++) {
    uint8_t buf[12000]; uint32_t len = (uint32_t)n;
    memcpy(buf, st, (size_t)n);
    int mode = it % 4;
    if (mode == 0) { int k = 1 + (int)(rnd() % 8); while (k--) buf[rnd() % (uint32_t)n] ^= (uint8_t)(1u << (rnd() % 8)); }
    else if (mode == 1) { len = rnd() % (uint32_t)n; }
    else if (mode == 2) { len = 1 + rnd() % 3000; for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)rnd(); }
    else { for (int k = 0; k < 4; k++) buf[rnd() % (uint32_t)n] = (uint8_t)rnd(); len = (uint32_t)n - rnd() % 5; }
    memcpy(fl + 0x3C100, buf, len);
    int r = lz_check(0x3C100, len, (it & 1), 50, 200);
    if (r) ok++; else bad++;
    lz_begin(0x3C100, len, (it >> 1) & 1, 50);
    uint8_t ln[50]; for (int y = 0; y < 210; y++) if (!lz_line(ln)) break;
  }
  printf("   fuzz: %ld accepted, %ld refused\n", ok, bad);
  // edge: stream at the very end of the flash, lengths 0 and 1, address beyond the end
  lz_check(0x7FFF0, 16, 0, 50, 200); lz_check(0x7FFF8, 100, 0, 50, 200); lz_check(0, 0, 0, 50, 200); lz_check(0x7FFFF, 1, 1, 50, 200); lz_begin(0x80000, 5, 0, 50);
  uint8_t ln[50]; lz_line(ln);
  xf_end();
  CK(flash_idle(), "flash closed after the LZ test");
}

// ---------------------------------------------------------------- T2: settings
static settings_t rand_set(void)
{
  settings_t s; memset(&s, 0, sizeof s);
  s.ss_en = rnd() & 1; s.ss_every = (uint16_t)(1 + rnd() % 1080); s.ss_mask = rnd(); s.orient = rnd() & 3; s.od_slot = rnd() % 32;
  s.lut = (uint8_t)rnd(); s.txp = rnd() % 6; s.led = rnd() % 4; s.opmode = rnd() & 1; s.wt = (uint8_t)(10 + rnd() % 241); s.wb = (uint8_t)(10 + rnd() % 241);
  s.tap_next = rnd() & 1; s.clean_n = (uint8_t)rnd(); s.clean_mode = rnd() % 3;
  int nl = (int)(rnd() % 6); for (int i = 0; i < nl; i++) s.name[i] = (char)(0x21 + rnd() % 0x5E);
  s.bt_en = rnd() & 1; s.bt_itv = rnd() & 3; s.bt_mask = (uint8_t)(1 + rnd() % 127); s.splash_off = rnd() & 1; s.bt_win = rnd() & 1; s.ss_order = rnd() & 1;
  s.amber_g = (uint8_t)rnd(); s.bc_preset = rnd() % 6; s.bc_col = (uint8_t)(1 + rnd() % 7); s.bc_lvl = (uint8_t)(1 + rnd() % 4); s.bc_dur = rnd() % 5; s.bc_keep = rnd() & 1;
  return s;
}
static bool same_set(const settings_t *a, const settings_t *b) { return memcmp(a, b, sizeof *a) == 0; }
static void set_in_limits(const char *tag)
{
  const settings_t *s = &g_set; (void)tag;
  bool ok = s->ss_every >= 1 && s->ss_every <= 1080 && s->od_slot <= 31 && s->txp <= 5 && s->led <= 3 && s->wt >= 10 && s->wt <= 250 && s->wb >= 10 && s->wb <= 250 &&
            s->clean_mode <= 2 && s->bc_preset <= 5 && s->bc_col >= 1 && s->bc_col <= 7 && s->bc_lvl >= 1 && s->bc_lvl <= 4 && s->bc_dur <= 4 && s->bt_mask != 0 && s->name[5] == 0;
  CK(ok, "loaded settings are inside their limits");
}
static void t_settings(void)
{
  printf("T2 settings\n");
  fl_reset();
  settings_load();
  settings_t def = g_set;
  CK(def.wt == 60 && def.amber_g == 150 && def.bc_lvl == 3, "defaults on blank flash");
  CK(flash_idle(), "flash closed after load");
  // round trip
  for (int i = 0; i < 2000; i++) {
    settings_t s = rand_set(); g_set = s;
    CK(settings_save(), "save ok");
    memset(&g_set, 0xEE, sizeof g_set);
    settings_load();
    if (!same_set(&g_set, &s)) { CK(0, "round trip differs"); break; }
    if (i % 400 == 0) CK(flash_idle(), "flash closed after save+load");
  }
  // wear: 300 000 saves, sector erase counts, no write-without-erase
  fl_reset(); settings_load();
  uint32_t saves = 300000;
  for (uint32_t i = 0; i < saves; i++) {
    g_set = rand_set();
    if (!settings_save()) { CK(0, "save failed"); break; }
    if (i % 50021 == 0) { settings_t s = g_set; settings_load(); CK(same_set(&g_set, &s), "state survives a reload in the middle of a run"); }
  }
  printf("   after %u saves: erases A=%u B=%u (expected about %u each), violations=%d, no-WEL=%d, ops below 0x3C000=%u\n", saves, fl_erase_cnt[0x7D], fl_erase_cnt[0x7E], saves / 128 / 2, fl_violations, fl_no_wel, fl_below_slots_ops);
  CK(fl_violations == 0 && fl_no_wel == 0, "no write without erase / without WEL");
  CK(fl_erase_cnt[0x7D] + fl_erase_cnt[0x7E] <= saves / 128 + 2, "erase count matches 128 records per sector");
  CK(fl_below_slots_ops == 0, "settings never touch the OTA area");
  CK(flash_idle(), "flash closed");
  // power cut at every possible op, many random states
  fl_reset(); settings_load();
  int torn_accept = 0, garbage = 0;
  for (int it = 0; it < 40000; it++) {
    settings_t before = rand_set(); g_set = before; fl_cut_budget = -1;
    if (it % 7 == 0) { for (int k = 0; k < 130; k++) { g_set = rand_set(); settings_save(); } g_set = before; }   // sometimes at a sector boundary
    settings_save(); settings_load(); before = g_set;
    settings_t next = rand_set(); g_set = next;
    fl_cut_budget = (int)(rnd() % 3);
    bool r = settings_save();
    fl_cut_budget = -1; fl_dead = false;
    (void)r;
    settings_load();
    set_in_limits("cut");
    if (same_set(&g_set, &next)) { /* the new record made it */ }
    else if (same_set(&g_set, &before)) { /* the old one stayed */ }
    else { garbage++; if (garbage <= 3) printf("   cut #%d: loaded record is neither old nor new (torn record accepted)\n", it); }
    // the next save after a cut must work and be read back
    settings_t again = rand_set(); g_set = again;
    if (!settings_save()) { CK(0, "save after a power cut fails"); break; }
    settings_load();
    if (!same_set(&g_set, &again)) { CK(0, "save after a power cut is not read back"); break; }
    (void)torn_accept;
  }
  printf("   power cuts: %d torn records accepted as valid out of 40000 cuts\n", garbage);
  CK(garbage == 0, "a power cut never produces a record that is neither old nor new");
  // legacy 'S1' record (v14 and older) is still read, and the next save continues after it
  fl_reset();
  { uint8_t r[32]; memset(r, 0, sizeof r); r[0] = 'S'; r[1] = '1'; put32(r + 2, 7); r[6] = 1; r[7] = 45; r[8] = 0; r[13] = 2; r[14] = 31; r[15] = 0xE4; r[16] = 3; r[17] = 2; r[19] = 33; r[20] = 99; r[29] = 0xFF; r[30] = 0x04;
    uint8_t c = 0x5A; for (int i = 0; i < 31; i++) c ^= r[i]; r[31] = c; memcpy(fl + 0x7D000, r, 32); }
  settings_load();
  CK(g_set.ss_en == 1 && g_set.ss_every == 45 && g_set.wt == 33 && g_set.wb == 99 && g_set.orient == 0 && g_set.splash_off == 1 && g_set.bt_en == 1, "S1 record is read (the old orientation 2 is dropped: v19 changed what it is relative to)");
  CK(g_set.amber_g == 150 && g_set.bc_preset == 0 && g_set.bc_lvl == 3, "S1 record: new fields get their defaults");
  CK(settings_seq() == 7, "S1 sequence");
  g_set.amber_g = 99; CK(settings_save(), "save after S1"); settings_load();
  CK(g_set.amber_g == 99 && g_set.ss_every == 45 && settings_seq() == 8, "S1 migrated by the next save");
  CK(fl[0x7D000 + 32] == 'S' && fl[0x7D000 + 33] == '3', "new record is S3 and sits after the old one");
  // an S3 record keeps the orientation, an S2 record does not
  g_set.orient = 3; CK(settings_save(), "save S3"); settings_load(); CK(g_set.orient == 3, "S3 keeps the orientation");
  { uint8_t r[32]; memset(r, 0, sizeof r); r[0] = 'S'; r[1] = '2'; put32(r + 2, 0x100); r[3] = 0x01; r[4] = 0; r[5] = 150; r[6] = 1; r[7] = 45; r[13] = 3; r[14] = 31; r[15] = 0xE4; r[16] = 3; r[17] = 2; r[19] = 33; r[20] = 99; r[29] = 0x7F; r[23] = 0x10;
    uint8_t c = 0x5A; for (int i = 0; i < 31; i++) c ^= r[i]; r[31] = c; fl_reset(); memcpy(fl + 0x7D000, r, 32); }
  settings_load(); CK(g_set.orient == 0 && g_set.ss_every == 45, "S2 record: orientation dropped, the rest kept");
  // power-up on blank flash with the flash chip missing (no answer): defaults, flash closed
  fl_reset(); fl_jedec[0] = 0xFF; fl_jedec[1] = 0xFF; fl_jedec[2] = 0xFF; settings_load(); CK(g_set.wt == 60, "no flash chip: defaults"); CK(!settings_save(), "no flash chip: save fails cleanly"); CK(spi_open_count == 0, "no flash chip: SPI closed");
  fl_jedec[0] = 0xA1; fl_jedec[1] = 0x28; fl_jedec[2] = 0x13;
  // flash stuck busy: erase and program time out instead of hanging
  fl_reset(); settings_load(); fl_stuck = 1; g_set = rand_set(); { long u0 = udelay_total_us; bool r = settings_save(); CK(!r, "stuck flash: save fails"); printf("   stuck flash: save gave up after %ld ms of polling\n", (udelay_total_us - u0) / 1000); } fl_stuck = 0;
  CK(spi_open_count == 0, "stuck flash: SPI closed again");
  settings_load(); g_set = rand_set(); CK(settings_save(), "flash works again afterwards");
  // noise in the settings sectors
  for (int it = 0; it < 20000; it++) {
    for (int k = 0; k < 64; k++) fl[0x7D000 + rnd() % 8192] = (uint8_t)rnd();
    if (it % 100 == 0) { memset(fl + 0x7D000, 0xFF, 8192); }
    settings_load(); set_in_limits("noise");
    g_set = rand_set(); settings_save();
  }
  CK(flash_idle(), "flash closed after noise test");
}

// ---------------------------------------------------------------- T3: slots
static uint8_t cmdbuf[300];
static void sc(uint8_t c, const uint8_t *p, int n) { cmdbuf[0] = c; if (n) memcpy(cmdbuf + 1, p, (size_t)n); slot_cmd(cmdbuf, (uint8_t)(1 + n)); }
static bool say_has(const char *s) { return strstr(say_last, s) != NULL; }
// uploads a stream: 42 slot len32 crc32 win fmt ; 43 seq data ; 44
static bool up_run(int slot, const uint8_t *st, uint32_t n, int fmt, int stop_at /* -1 whole, else bytes sent then walk away */, bool corrupt_crc)
{
  uint8_t b[16]; b[0] = (uint8_t)slot; put32(b + 1, n); put32(b + 5, crc32(st, n) ^ (corrupt_crc ? 1u : 0u)); b[9] = 8; b[10] = (uint8_t)fmt;
  sc(0x42, b, 11);
  if (!say_has("UP READY")) return false;
  uint32_t off = 0; uint16_t seq = 0;
  while (off < n) {
    if (stop_at >= 0 && (int)off >= stop_at) return false;
    uint32_t k = n - off > 200 ? 200 : n - off;
    uint8_t d[210]; d[0] = (uint8_t)seq; d[1] = (uint8_t)(seq >> 8); memcpy(d + 2, st + off, k);
    sc(0x43, d, (int)k + 2);
    if (say_has("FAIL")) return false;
    off += k; seq++;
  }
  sc(0x44, NULL, 0);
  return say_has("UP OK");
}
static void t_slots(void)
{
  printf("T3 slots\n");
  fl_reset();
  settings_load();
  uint8_t fr[10000], st[12000];
  // garbage in the slot area: scan must survive
  for (int k = 0; k < 200000; k++) fl[0x3C000 + rnd() % 0x41000] = (uint8_t)rnd();
  sc(0x40, NULL, 0);
  CK(say_has("slots:"), "list on garbage works");
  CK(flash_idle(), "flash closed after list");
  sc(0x41, (const uint8_t[]){ 0xFF }, 1);
  CK(say_has("ERASED all"), "erase all");
  CK(flash_idle(), "flash closed after erase all");
  // fill, replace, erase in random order with random aborts
  int good = 0, aborted = 0, refused = 0;
  uint32_t crcs[32]; memset(crcs, 0, sizeof crcs); bool have[32] = { 0 };
  for (int it = 0; it < 6000; it++) {
    int slot = (int)(rnd() % 32);
    int act = (int)(rnd() % 10);
    if (act < 6) {
      int kind = (int)(rnd() % 5), fmt = (int)(rnd() % 3);
      make_frame(kind, fr);
      const uint8_t *src = fr; uint32_t n;
      if (fmt == 0) { n = 10000; memcpy(st, fr, n); }
      else {
        uint8_t xr[10000]; if (fmt == 2) { for (int y = 0; y < 200; y++) for (int x = 0; x < 50; x++) xr[y * 50 + x] = (uint8_t)(fr[y * 50 + x] ^ (y ? fr[(y - 1) * 50 + x] : 0)); src = xr; }
        n = (uint32_t)lz4_enc(src, 10000, st);
      }
      int stop = (rnd() % 8 == 0) ? (int)(rnd() % n) : -1;
      bool corrupt = rnd() % 25 == 0;
      bool ok = up_run(slot, st, n, fmt, stop, corrupt);
      if (stop >= 0) {
        aborted++;
        int how = (int)(rnd() % 3);
        if (how == 0) sc(0x49, NULL, 0); else if (how == 1) slot_link_closed(); /* else: the next command arrives with the upload still open */
        if (how != 2) CK(!slot_busy(), "no upload after abort / link close");
        have[slot] = false;
        if (how == 2) { sc(0x49, NULL, 0); }
      } else if (ok) { good++; have[slot] = true; crcs[slot] = crc32(st, n); if (!corrupt) {} else CK(0, "corrupt CRC accepted"); }
      else { refused++; have[slot] = false; if (!corrupt && !say_has("no room")) { if (!say_has("room")) { printf("   unexpected refusal: %s\n", say_last); CK(0, "upload refused without a reason"); } } }
      CK(!slot_busy(), "upload closed at the end of an upload attempt");
    } else if (act < 8) {
      uint8_t a = (uint8_t)slot; sc(0x41, &a, 1); have[slot] = false;
    } else if (act == 8) {
      uint8_t a = (uint8_t)slot; sc(0x47, &a, 1);
      if (have[slot]) CK(say_has("ok"), "verify of a filled slot");
    } else {
      sc(0x40, NULL, 0);
    }
    if (!flash_idle()) { printf("   flash left open after step %d (act %d): %s\n", it, act, say_last); CK(0, "flash left open"); break; }
  }
  // slot directory must equal what was stored (rescan from flash by listing after reload is done in a separate process)
  printf("   uploads ok=%d aborted=%d refused=%d, violations=%d, no-WEL=%d, ops below 0x3C000=%u\n", good, aborted, refused, fl_violations, fl_no_wel, fl_below_slots_ops);
  CK(fl_violations == 0 && fl_no_wel == 0, "no write without erase");
  CK(fl_below_slots_ops == 0, "slides never touch the OTA area");
  uint32_t mx = 0, mn = ~0u, sum = 0; for (int i = 0x3C; i < 0x7D; i++) { if (fl_erase_cnt[i] > mx) mx = fl_erase_cnt[i]; if (fl_erase_cnt[i] < mn) mn = fl_erase_cnt[i]; sum += fl_erase_cnt[i]; }
  printf("   erase counts of slide sectors: min %u max %u total %u; settings sectors %u/%u; sector 0x7F %u\n", mn, mx, sum, fl_erase_cnt[0x7D], fl_erase_cnt[0x7E], fl_erase_cnt[0x7F]);
  uint32_t o = 0; for (int i = 0; i < 0x3C; i++) o += fl_erase_cnt[i];
  CK(o == 0, "no erase in the OTA area by slides");
}

// year of slideshow in virtual time
static int draws_seen = 0;
static void t_show(void)
{
  printf("T3b slideshow, one virtual year\n");
  fl_reset(); settings_load();
  uint8_t fr[10000], st[12000];
  for (int s = 0; s < 6; s++) { make_frame(s % 5, fr); uint32_t n = (uint32_t)lz4_enc(fr, 10000, st); CK(up_run(s, st, n, 1, -1, false), "upload for show"); }
  for (int per = 1; per <= 1080; per = per < 60 ? per * 7 : (per == 63 ? 1080 : per * 3)) {
    g_set.ss_en = 1; g_set.ss_every = (uint16_t)per; g_set.ss_order = (uint8_t)(per & 1); g_set.clean_n = 0;
    slot_show_arm();
    int e0 = vt_start_errors; slot_poll();
    uint64_t total_ms = 365ULL * 24 * 3600 * 1000;
    uint32_t cnt = 0; (void)cnt;
    // main loop emulation: poll after each event (plus every second to take draws)
    uint64_t t = 0; int slides = 0; int cur_before = slot_current();
    (void)cur_before;
    while (t < total_ms) {
      vt_advance_ms(per >= 60 ? 60000 : 20000); t += per >= 60 ? 60000 : 20000;
      int pend = slot_draw_pending();
      slot_poll();
      if (pend) slides++;
    }
    int expect = (int)(total_ms / ((uint64_t)per * 60000));
    printf("   every %4d min: %d slideshow steps in a year (expected about %d), timer start errors %d\n", per, slides, expect, vt_start_errors - e0);
    CK(vt_start_errors == e0, "no failed timer starts in the slideshow");
    CK(slides >= expect * 95 / 100 && slides <= expect * 105 / 100 + 2, "slideshow keeps its pace for a year");
    g_set.ss_en = 0; slot_show_arm();
    CK(vt_running_timers() == 0, "slideshow timer stopped when disabled");
    CK(flash_idle(), "flash closed during the year");
  }
  (void)draws_seen;
}

// ---------------------------------------------------------------- T4: OTA
// A made-up GBL file with real tag structure: header tag, metadata tag (product mark), one big data tag, End tag with the CRC-32
// (the CRC runs over everything up to and including the End tag's own CRC field).
// kind 0 good | 1 other product | 2 data damaged after the CRC was made | 3 good, product mark deep inside the data tag instead of the metadata tag
// 4 good with 37 bytes after the End tag (ignored by the bootloader) | 5 End tag missing
static uint8_t *make_gbl(int kind, long *n)
{
  uint32_t payload = 150000, pad = kind == 4 ? 37 : 0, o = 0;
  uint32_t len = 16 + 16 + 8 + payload + 12 + pad;
  uint8_t *b = malloc(len);
  for (uint32_t i = 0; i < len; i++) b[i] = (uint8_t)rnd();
  put32(b + o, 0x03A617EBUL); put32(b + o + 4, 8); put32(b + o + 8, 0x03000000UL); put32(b + o + 12, 0); o += 16;
  put32(b + o, 0xF60A0AF6UL); put32(b + o + 4, 8); memcpy(b + o + 8, kind == 1 ? "OTHERPRD" : kind == 3 ? "\x01\x02\x03\x04\x05\x06\x07\x08" : "BG22BADG", 8); o += 16;
  put32(b + o, 0xF40A0AF4UL); put32(b + o + 4, payload); o += 8;
  if (kind == 3) memcpy(b + 100003, "BG22BADG", 8);
  o += payload;
  put32(b + o, 0xFC0404FCUL); put32(b + o + 4, 4); put32(b + o + 8, crc32(b, o + 8));
  if (pad) memset(b + o + 12, 0xFF, pad);
  if (kind == 2) b[5000] ^= 1;
  if (kind == 5) put32(b + o, 0x11111111UL);
  *n = (long)len;
  return b;
}
static void oc(const uint8_t *p, int n) { ota_cmd(p, (uint8_t)n); }
static void ob(uint8_t sub, uint32_t len, uint32_t crc, uint8_t win)
{
  uint8_t b[12] = { 0x46, 1 }; put32(b + 2, len); put32(b + 6, crc); b[10] = win; (void)sub; tapgate_touch(); oc(b, 11);
}
static bool ota_send(const uint8_t *img, uint32_t n, uint32_t from, uint32_t to, uint32_t chunk, uint16_t *seqp)
{
  uint32_t off = from;
  while (off < to) {
    uint32_t k = to - off > chunk ? chunk : to - off;
    uint8_t d[260]; d[0] = 0x46; d[1] = 2; d[2] = (uint8_t)*seqp; d[3] = (uint8_t)(*seqp >> 8); memcpy(d + 4, img + off, k);
    oc(d, (int)k + 4);
    if (strstr(say_last, "FAIL")) return false;
    off += k; (*seqp)++;
  }
  (void)n; return true;
}
static void ota_state_clean(const char *where)
{
  char m[160];
  snprintf(m, sizeof m, "after %s: flash closed", where); CK(flash_idle(), m);
  snprintf(m, sizeof m, "after %s: EM1 requirement released (pm_em1=%d)", where, pm_em1); CK(pm_em1 == 0, m);
  snprintf(m, sizeof m, "after %s: no atomic section left open (%d)", where, core_depth); CK(core_depth == 0, m);
  snprintf(m, sizeof m, "after %s: LED engine idle", where); CK(!led_busy(), m);
  snprintf(m, sizeof m, "after %s: ota not busy", where); CK(!ota_busy(), m);
}
static void t_ota(void)
{
  printf("T4 OTA\n");
  long n1, n2, n3, n5;
  uint8_t *good = make_gbl(0, &n1), *other = make_gbl(1, &n2), *bad = make_gbl(2, &n3), *noend = make_gbl(5, &n5);
  fl_reset(); settings_load(); led_init(); g_set.led = 2;
  // some slides + settings before the update (must survive)
  uint8_t fr[10000], st[12000]; make_frame(2, fr); uint32_t sn = (uint32_t)lz4_enc(fr, 10000, st);
  for (int s = 0; s < 4; s++) CK(up_run(s, st, sn, 1, -1, false), "slide upload before OTA");
  g_set.amber_g = 77; settings_save();
  static uint8_t snap[0x80000 - 0x3C000]; memcpy(snap, fl + 0x3C000, sizeof snap);
  uint32_t gcrc = crc32(good, (uint32_t)n1);

  // --- complete good run
  stub_vbat = 3100; stub_mtu = 247;
  ob(0, (uint32_t)n1, gcrc, 8);
  CK(strstr(say_last, "OTA READY") != NULL, "begin accepted");
  unsigned chunk = 240; sscanf(strstr(say_last, "chunk=") + 6, "%u", &chunk);
  CK(chunk == 236, "chunk size 236 for MTU 247");
  CK(ota_busy(), "busy while uploading");
  CK(fl_powered_down == false && spi_open_count == 1, "flash open during upload");
  CK(led_pulsing(), "magenta pulse during upload");
  uint16_t seq = 0; CK(ota_send(good, (uint32_t)n1, 0, (uint32_t)n1, chunk, &seq), "all chunks accepted");
  vt_advance_ms(2000);
  oc((const uint8_t[]){ 0x46, 3 }, 2);
  CK(strstr(say_last, "OTA OK") != NULL, "verify ok");
  CK(memcmp(fl, good, (size_t)n1) == 0, "image in flash equals the file");
  for (uint32_t i = (uint32_t)n1; i < 0x3B000; i++) if (fl[i] != 0xFF && i >= ((n1 + 4095) & ~4095u)) { CK(0, "stale bytes after the image sectors"); break; }
  CK(memcmp(snap, fl + 0x3C000, sizeof snap) == 0, "slides and settings untouched by the update");
  CK(spi_open_count == 0, "flash closed after verify");
  oc((const uint8_t[]){ 0x46, 4, 0x00 }, 3); CK(btl_reboot_install == 0 && strstr(say_last, "FAIL arg"), "install without A5 refused");
  oc((const uint8_t[]){ 0x46, 4, 0xA5 }, 3); CK(btl_reboot_install == 0 && stub_reboot_calls == 1, "install asks for the bootloader reboot");
  ota_link_closed();
  ota_state_clean("complete run");
  CK(fl_violations == 0 && fl_no_wel == 0, "no write without erase during OTA");

  // --- failures of begin
  struct { uint32_t len, crc; uint16_t mv; const char *why; } B[] = {
    { 10, 0, 3000, "size" }, { 0x3B001, 0, 3000, "size" }, { 0xFFFFFFFF, 0, 3000, "size" }, { 150052, 0, 2600, "battery" } };
  for (unsigned i = 0; i < sizeof B / sizeof B[0]; i++) {
    stub_vbat = B[i].mv; ob(0, B[i].len, B[i].crc, 8);
    CK(strstr(say_last, "FAIL") && strstr(say_last, B[i].why), "begin refused");
    ota_state_clean("refused begin");
  }
  stub_vbat = 3000;
  oc((const uint8_t[]){ 0x46, 1, 1, 2 }, 4); CK(strstr(say_last, "short"), "short begin");
  ota_state_clean("short begin");
  oc((const uint8_t[]){ 0x46, 0x99 }, 2); CK(strstr(say_last, "ERR"), "unknown subcommand");
  oc((const uint8_t[]){ 0x46 }, 1); CK(strstr(say_last, "short"), "no subcommand");
  oc((const uint8_t[]){ 0x46, 2, 0, 0, 1 }, 5); CK(strstr(say_last, "no upload"), "data without begin");
  oc((const uint8_t[]){ 0x46, 3 }, 2); CK(strstr(say_last, "no upload"), "end without begin");
  oc((const uint8_t[]){ 0x46, 4, 0xA5 }, 3); CK(strstr(say_last, "not verified"), "install without verify");
  oc((const uint8_t[]){ 0x46, 5 }, 2); CK(strstr(say_last, "idle"), "abort when idle");
  ota_state_clean("misc commands");

  // --- wrong files
  uint8_t *imgs[3] = { other, bad, noend }; long ns[3] = { n2, n3, n5 }; const char *why[3] = { "wrong product", "gbl crc", "no end tag" };
  for (int k = 0; k < 3; k++) {
    ob(0, (uint32_t)ns[k], crc32(imgs[k], (uint32_t)ns[k]), 8); seq = 0;
    CK(ota_send(imgs[k], (uint32_t)ns[k], 0, (uint32_t)ns[k], 236, &seq), "upload of a foreign file");
    oc((const uint8_t[]){ 0x46, 3 }, 2);
    char m[220]; snprintf(m, sizeof m, "refused: %s (got '%s')", why[k], say_last); CK(strstr(say_last, why[k]) != NULL, m);
    ota_state_clean("wrong file");
    oc((const uint8_t[]){ 0x46, 4, 0xA5 }, 3); CK(stub_reboot_calls == 1, "never installs a refused file");
  }
  // announced CRC wrong
  ob(0, (uint32_t)n1, gcrc ^ 1, 8); seq = 0; ota_send(good, (uint32_t)n1, 0, (uint32_t)n1, 236, &seq); oc((const uint8_t[]){ 0x46, 3 }, 2);
  CK(strstr(say_last, "FAIL crc") != NULL, "announced CRC mismatch"); ota_state_clean("crc mismatch");
  // product mark far inside the file (metadata tag placed after the header), also straddling a 256-byte read chunk
  for (int off_k = 0; off_k < 2; off_k++) {
    long n4; uint8_t *late = make_gbl(3, &n4);
    if (off_k) { memcpy(late + 100003, "\x11\x22\x33\x44\x55\x66\x77\x88", 8); memcpy(late + 100096 + 252, "BG22BADG", 8); put32(late + n4 - 4, crc32(late, (uint32_t)n4 - 4)); }
    ob(0, (uint32_t)n4, crc32(late, (uint32_t)n4), 8); seq = 0; ota_send(late, (uint32_t)n4, 0, (uint32_t)n4, 236, &seq); oc((const uint8_t[]){ 0x46, 3 }, 2);
    CK(strstr(say_last, "OTA OK") != NULL, "product mark found far inside the file / across a chunk boundary");
    ota_link_closed(); ota_state_clean("late mark"); free(late);
  }
  // too long
  ob(0, 1000, 0, 8); seq = 0; ota_send(good, 2000, 0, 2000, 236, &seq); CK(strstr(say_last, "too long") != NULL, "too much data"); ota_state_clean("too long");
  // wrong sequence
  ob(0, (uint32_t)n1, gcrc, 8); seq = 0; ota_send(good, (uint32_t)n1, 0, 472, 236, &seq); seq = 5; ota_send(good, (uint32_t)n1, 472, 708, 236, &seq);
  CK(strstr(say_last, "seq") != NULL, "sequence error"); ota_state_clean("seq error");
  // incomplete
  ob(0, (uint32_t)n1, gcrc, 8); seq = 0; ota_send(good, (uint32_t)n1, 0, 4720, 236, &seq); oc((const uint8_t[]){ 0x46, 3 }, 2);
  CK(strstr(say_last, "incomplete") != NULL, "incomplete image"); ota_state_clean("incomplete");
  // battery drops before install
  ob(0, (uint32_t)n1, gcrc, 8); seq = 0; ota_send(good, (uint32_t)n1, 0, (uint32_t)n1, 236, &seq); oc((const uint8_t[]){ 0x46, 3 }, 2);
  CK(strstr(say_last, "OTA OK") != NULL, "second good run");
  stub_vbat = 2650; oc((const uint8_t[]){ 0x46, 4, 0xA5 }, 3); CK(strstr(say_last, "battery") && stub_reboot_calls == 1, "install refused at low battery");
  stub_vbat = 3000; ota_link_closed(); ota_state_clean("low battery install");
  CK(memcmp(snap, fl + 0x3C000, sizeof snap) == 0, "slides untouched after all the failed attempts");
  CK(fl_below_slots_ops > 0, "OTA did write below 0x3C000 (expected)");

  // --- abort at every position (every chunk), by abort command, link close and by a slide command
  int cuts = 0;
  for (uint32_t at = 0; at < (uint32_t)n1; at += 236 * 37 + 17) {
    for (int how = 0; how < 4; how++) {
      ob(0, (uint32_t)n1, gcrc, 8); seq = 0; ota_send(good, (uint32_t)n1, 0, at, 236, &seq);
      if (how == 0) oc((const uint8_t[]){ 0x46, 5 }, 2);
      else if (how == 1) ota_link_closed();
      else if (how == 2) ob(0, (uint32_t)n1, gcrc, 8);                 // a second begin restarts
      else { uint8_t d[8] = { 0x46, 2, 0, 0 }; oc(d, 3); }               // a malformed data frame
      if (how == 2) { CK(strstr(say_last, "READY") != NULL, "restart"); ota_link_closed(); }
      if (how == 3) { CK(strstr(say_last, "FAIL") != NULL, "short data refused"); }
      ota_state_clean("abort"); cuts++;
    }
  }
  printf("   %d aborts at different positions, all left flash closed, EM1 balanced, LED stopped\n", cuts);
  // write protection of the OTA area outside an OTA run
  uint8_t x = 0x12; uint8_t id[3]; xf_begin(id);
  CK(!xf_write(0x1000, &x, 1) && !xf_erase_sector(0x1000) && !xf_write(0x3B800, &x, 1), "normal mode refuses writes below 0x3C000");
  xf_ota_mode(true);
  CK(!xf_write(0x3C000, &x, 1) && !xf_erase_sector(0x3C000) && !xf_write(0x3B000, &x, 1) && !xf_erase_sector(0x7D000), "OTA mode refuses writes above 0x3B000");
  CK(xf_erase_sector(0x3A000) && xf_write(0x3AFFF, &x, 1) && !xf_write(0x3AFFF, &x, 2), "OTA mode allows the OTA area, exactly up to its end");
  xf_ota_mode(false); xf_end();
  CK(flash_idle(), "flash closed");
  CK(fl_violations == 0 && fl_no_wel == 0, "no write without erase");
  // good file with bytes after the End tag (the bootloader ignores them)
  { long n4; uint8_t *pd = make_gbl(4, &n4); ob(0, (uint32_t)n4, crc32(pd, (uint32_t)n4), 8); seq = 0; ota_send(pd, (uint32_t)n4, 0, (uint32_t)n4, 236, &seq); oc((const uint8_t[]){ 0x46, 3 }, 2);
    CK(strstr(say_last, "OTA OK") != NULL, "bytes after the End tag are tolerated"); ota_link_closed(); ota_state_clean("padded file"); free(pd); }
  free(good); free(other); free(bad); free(noend);
}

// ---------------------------------------------------------------- T5: beacon / LED over virtual time
static double led_duty_s = 0, em1_s = 0, lit_s = 0, pd = 0; static int pem = 0; static uint64_t last_us = 0;
static void meter_hook(void)
{
  uint64_t t = vt_now_us(); double dt = (double)(t - last_us) / 1e6; last_us = t;
  led_duty_s += pd * dt; if (pd > 0) lit_s += dt; if (pem > 0) em1_s += dt;     // the state that held since the previous event
  uint32_t en = gpio_regs.TIMERROUTE[1].ROUTEEN; pd = 0;
  if (timer_enabled) for (int c = 0; c < 3; c++) if (en & (1u << c)) pd += (double)timer_cmp[c] / 1000.0;
  pem = pm_em1;
  beacon_poll();
}
static void meter_reset(void) { led_duty_s = em1_s = lit_s = 0; pd = 0; pem = pm_em1; last_us = vt_now_us(); }
static void adv(uint64_t ms) { uint64_t step = 1000; while (ms) { uint64_t d = ms < step ? ms : step; uint64_t t0 = vt_now_us(); vt_advance_ms(d); uint64_t t1 = vt_now_us(); (void)t0; (void)t1; ms -= d; double dt = (double)d / 1000.0; (void)dt; } }
static void t_beacon(void)
{
  printf("T5 beacon / LED\n");
  fl_reset(); settings_load(); led_init(); g_set.led = 2;
  vt_hook = meter_hook; stub_vbat = 3000;
  static const char *NM[] = { "", "locator", "heartbeat", "SOS", "rainbow", "breathing" };
  for (int wrap = 0; wrap < 2; wrap++) {
    if (wrap) vt_set_ms(0xFFFFFFFFULL - 5400ULL * 1000);        // 90 minutes before the 32-bit millisecond counter wraps (49.7 days)
    for (int p = 1; p <= 5; p++) {
      int e0 = vt_start_errors;
      meter_reset(); last_us = vt_now_us();
      CK(beacon_start((uint8_t)p, LC_GREEN, 4, 4), "start");
      uint8_t md = beacon_max_dur((uint8_t)p);
      const uint32_t H[5] = { 1, 6, 24, 72, 168 };
      uint64_t cap_ms = (uint64_t)H[md] * 3600000ULL;
      CK(beacon_left_s() > (cap_ms / 1000) - 5 && beacon_left_s() <= cap_ms / 1000, "left time at start equals the preset cap");
      uint64_t t0 = vt_now_us(); uint64_t elapsed = 0; bool still = true;
      while (elapsed < cap_ms + 120000ULL) {
        vt_advance_ms(5000); elapsed += 5000;
        if (still && !beacon_active()) { still = false; break; }
      }
      uint64_t ran = (vt_now_us() - t0) / 1000;
      char m[160];
      snprintf(m, sizeof m, "%s%s stops at its cap (ran %.3f h, cap %u h)", NM[p], wrap ? " across the ms wrap" : "", (double)ran / 3.6e6, H[md]);
      CK(!beacon_active() && ran >= cap_ms - 1000 && ran <= cap_ms + 40000, m);
      double hours = (double)ran / 3.6e6;
      printf("   %-9s %s cap %3u h: LED duty-equivalent %.3f%% , EM1 held %.2f%% of the time\n", NM[p], wrap ? "(wrap)" : "      ", H[md], 100.0 * led_duty_s / (hours * 3600), 100.0 * em1_s / (hours * 3600));
      vt_advance_ms(3000);
      snprintf(m, sizeof m, "%s: everything released after the stop (em1=%d timers=%d led_busy=%d bg=%d)", NM[p], pm_em1, vt_running_timers(), led_busy(), led_bg_running());
      CK(pm_em1 == 0 && vt_running_timers() == 0 && !led_busy() && !led_bg_running() && !timer_enabled, m);
      CK(vt_start_errors == e0, "no failed timer start");
    }
  }
  // restart replaces: one timer only
  CK(beacon_start(BC_LOCATOR, 1, 2, 0), "start"); CK(beacon_start(BC_HEARTBEAT, 2, 2, 0), "restart other preset"); CK(beacon_start(BC_BREATH, 3, 2, 0), "restart again");
  adv(10000);
  int tm = vt_running_timers(); CK(tm <= 2, "at most the beacon timer and the LED timer run");
  beacon_stop(); CK(vt_running_timers() == 0 && pm_em1 == 0, "all stopped"); 
  // battery guard
  CK(beacon_start(BC_LOCATOR, 1, 3, 4), "start"); adv(120000);
  stub_vbat = 2590; adv(31000);
  CK(!beacon_active(), "stops by itself below 2.6 V (reading taken under LED load)"); vt_advance_ms(3000); CK(pm_em1 == 0 && !led_busy(), "dark after the guard");
  stub_vbat = 2790; CK(!beacon_start(BC_LOCATOR, 1, 3, 4), "no start below 2.8 V"); stub_vbat = 3000;
  // hold / resume (panel draw)
  CK(beacon_start(BC_RAINBOW, 1, 3, 1), "start"); adv(20000);
  beacon_hold(true); led_stop(); meter_reset(); adv(60000);
  CK(led_duty_s == 0, "no light while held"); beacon_hold(false); adv(20000); CK(led_duty_s > 0, "light again after the hold");
  beacon_stop();
  // an event flash in the middle of every preset: the preset continues afterwards
  for (int p = 1; p <= 5; p++) {
    CK(beacon_start((uint8_t)p, 2, 2, 0), "start"); adv(7000);
    led_flash(LC_BLUE, 2, 40, 120); adv(300); led_pulse(LC_MAGENTA, 1600, 200); adv(5000); led_stop(); meter_reset(); adv(40000);
    char m[100]; snprintf(m, sizeof m, "%s continues after an event", NM[p]); CK(led_duty_s > 0, m);
    beacon_stop(); CK(pm_em1 == 0 && vt_running_timers() == 0, "stopped clean");
  }
  // keep across restarts: beacon_boot
  g_set.bc_keep = 1; g_set.bc_preset = BC_HEARTBEAT; g_set.bc_col = 1; g_set.bc_lvl = 3; g_set.bc_dur = 1;
  beacon_boot(true); CK(!beacon_active(), "no restart after a crash"); beacon_boot(false); CK(beacon_active(), "restart after a clean reset"); beacon_stop();
  g_set.bc_keep = 0; beacon_boot(false); CK(!beacon_active(), "no restart without keep");
  // random storm: commands from "interrupt" and "main" in random order for virtual days
  uint32_t ops = 0;
  for (int it = 0; it < 200000; it++) {
    switch (rnd() % 12) {
      case 0: beacon_start((uint8_t)(rnd() % 7), (uint8_t)(rnd() % 9), (uint8_t)(rnd() % 6), (uint8_t)(rnd() % 7)); break;
      case 1: beacon_stop(); break;
      case 2: beacon_hold(rnd() & 1); break;
      case 3: led_flash((uint8_t)(rnd() % 8), (uint8_t)(rnd() % 4), (uint16_t)(rnd() % 300), (uint16_t)(rnd() % 300)); break;
      case 4: led_pulse((uint8_t)(rnd() % 8), (uint16_t)(rnd() % 3000), (uint8_t)rnd()); break;
      case 5: led_stop(); break;
      case 6: led_rgb((uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd(), (uint16_t)(rnd() % 500)); break;
      case 7: stub_vbat = (uint16_t)(2500 + rnd() % 800); break;
      case 8: led_pulse_bg((uint8_t)(rnd() % 8), (uint16_t)(rnd() % 6000), (uint8_t)rnd()); break;
      default: vt_advance_ms(rnd() % 4000); break;
    }
    ops++;
    if (core_depth != 0) { CK(0, "atomic section left open"); break; }
  }
  beacon_stop(); led_stop(); beacon_hold(false); stub_vbat = 3000; vt_advance_ms(10000);
  CK(vt_running_timers() == 0 && pm_em1 == 0 && !timer_enabled && cmu_timer1 == 0, "after the storm everything is released");
  printf("   storm: %u random operations, start errors total %d, max atomic depth %d, max EM1 %d\n", ops, vt_start_errors, core_max_depth, pm_em1_max);
  vt_hook = NULL;
}

void t_panel_all(void);
void panel_test_select(int which);
int panel_test_checks(void), panel_test_fails(void);


// ---------------------------------------------------------------- T9: NDEF messages for the NFC chip
static const uint8_t WIFI_HOME[] = { 0xD2,0x17,0x3C,0x61,0x70,0x70,0x6C,0x69,0x63,0x61,0x74,0x69,0x6F,0x6E,0x2F,0x76,0x6E,0x64,0x2E,0x77,0x66,0x61,0x2E,0x77,0x73,0x63,0x10,0x4A,0x00,0x01,0x10,0x10,0x0E,0x00,0x33,0x10,0x26,0x00,0x01,0x01,0x10,0x45,0x00,0x07,0x48,0x6F,0x6D,0x65,0x4E,0x65,0x74,0x10,0x03,0x00,0x02,0x00,0x20,0x10,0x0F,0x00,0x02,0x00,0x08,0x10,0x27,0x00,0x09,0x73,0x65,0x63,0x72,0x65,0x74,0x31,0x32,0x33,0x10,0x20,0x00,0x06,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
static const uint8_t WIFI_OPEN[] = { 0xD2,0x17,0x30,0x61,0x70,0x70,0x6C,0x69,0x63,0x61,0x74,0x69,0x6F,0x6E,0x2F,0x76,0x6E,0x64,0x2E,0x77,0x66,0x61,0x2E,0x77,0x73,0x63,0x10,0x4A,0x00,0x01,0x10,0x10,0x0E,0x00,0x27,0x10,0x26,0x00,0x01,0x01,0x10,0x45,0x00,0x04,0x43,0x61,0x66,0x65,0x10,0x03,0x00,0x02,0x00,0x01,0x10,0x0F,0x00,0x02,0x00,0x01,0x10,0x27,0x00,0x00,0x10,0x20,0x00,0x06,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

static void t_ndef(void)
{
  printf("T9 NDEF\n");
  uint8_t m[NDEF_MAX + 64];
  char w[NDEF_MAX];
  size_t n;
  // URI
  n = ndef_uri(m, "https://steanlab.github.io/pin16/");
  CK(n == 5 + 33 && m[0] == 0xD1 && m[1] == 1 && m[2] == 1 + 33 && m[3] == 'U' && m[4] == 0, "URI header");
  CK(ndef_parse(m, n, w, sizeof w) == NDEF_URI && !strcmp(w, "https://steanlab.github.io/pin16/"), "URI round trip");
  { char u[NDEF_URL_MAX + 2]; memset(u, 'a', sizeof u); u[NDEF_URL_MAX] = 0; CK(ndef_uri(m, u) == 5 + NDEF_URL_MAX, "URI of the longest size"); u[NDEF_URL_MAX] = 'a'; u[NDEF_URL_MAX + 1] = 0; CK(ndef_uri(m, u) == 0, "URI one character too long"); }
  CK(ndef_uri(m, "") == 0 && ndef_uri(m, NULL) == 0 && ndef_uri(m, "caf\xC3\xA9") == 0 && ndef_uri(m, "a\tb") == 0, "URI refuses empty, NULL, non-ASCII, control characters");
  { const uint8_t pre[] = { 0xD1, 0x01, 0x0C, 'U', 0x04, 'e','x','a','m','p','l','e','.','o','r','g' };
    CK(ndef_parse(pre, sizeof pre, w, sizeof w) == NDEF_URI && !strcmp(w, "https://example.org"), "URI prefix code 4 is expanded");
    const uint8_t pre1[] = { 0xD1, 0x01, 0x06, 'U', 0x01, 'a','b','.','c','h' };
    CK(ndef_parse(pre1, sizeof pre1, w, sizeof w) == NDEF_URI && !strcmp(w, "http://www.ab.ch"), "URI prefix code 1 is expanded");
    const uint8_t pre9[] = { 0xD1, 0x01, 0x03, 'U', 0x7E, 'a','b' };
    CK(ndef_parse(pre9, sizeof pre9, w, sizeof w) == NDEF_URI && !strcmp(w, "ab"), "unknown prefix code gives no prefix"); }
  // text
  n = ndef_text(m, "Hello, world");
  CK(n == 7 + 12 && m[3] == 'T' && m[4] == 0x02 && m[5] == 'e' && m[6] == 'n', "text header (UTF-8, en)");
  CK(ndef_parse(m, n, w, sizeof w) == NDEF_TEXT && !strcmp(w, "Hello, world"), "text round trip");
  const char *u8[] = { "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82", "Zażółć gęślą jaźń", "\xE2\x82\xAC 5", "pin \xF0\x9F\x93\x8C" };    // Cyrillic, Polish, euro, pushpin
  for (int i = 0; i < 4; i++) { n = ndef_text(m, u8[i]); CK(n && ndef_parse(m, n, w, sizeof w) == NDEF_TEXT && !strcmp(w, u8[i]), "UTF-8 text round trip"); }
  { char t[NDEF_TEXT_MAX + 2]; memset(t, 'x', sizeof t); t[NDEF_TEXT_MAX] = 0; CK(ndef_text(m, t) == 7 + NDEF_TEXT_MAX, "text of the longest size"); t[NDEF_TEXT_MAX] = 'x'; t[NDEF_TEXT_MAX + 1] = 0; CK(ndef_text(m, t) == 0, "text one byte too long"); }
  const char *bad[] = { "", "\x80", "ab\xC3", "\xC0\x80", "\xC1\xBF", "\xE0\x80\x80", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xF8\x88\x80\x80\x80", "a\nb", "a\x7F", "\xE2\x82" };
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) CK(ndef_text(m, bad[i]) == 0, "invalid UTF-8 or control characters refused");
  CK(ndef_text(m, NULL) == 0, "text NULL");
  // Wi-Fi: exact bytes
  n = ndef_wifi(m, "HomeNet", "secret123", NDEF_AUTH_WPA2);
  CK(n == sizeof WIFI_HOME && !memcmp(m, WIFI_HOME, n), "Wi-Fi WPA2 message equals the reference bytes");
  n = ndef_wifi(m, "Cafe", "", NDEF_AUTH_OPEN);
  CK(n == sizeof WIFI_OPEN && !memcmp(m, WIFI_OPEN, n), "Wi-Fi open network equals the reference bytes");
  n = ndef_wifi(m, "HomeNet", "secret123", NDEF_AUTH_WPA2);
  CK(ndef_parse(m, n, w, sizeof w) == NDEF_WIFI && !strcmp(w, "HomeNet") && !strstr(w, "secret"), "Wi-Fi: only the network name is shown, never the key");
  { char s32[33], k63[64]; memset(s32, 'A', 32); s32[32] = 0; memset(k63, 'k', 63); k63[63] = 0;
    n = ndef_wifi(m, s32, k63, NDEF_AUTH_WPA_WPA2);
    CK(n == 165 && n <= NDEF_MAX && m[2] == 0x8B && ndef_parse(m, n, w, sizeof w) == NDEF_WIFI && !strcmp(w, s32), "Wi-Fi of the largest size");
    char s33[34]; memset(s33, 'A', 33); s33[33] = 0; char k64[65]; memset(k64, 'k', 64); k64[64] = 0;
    CK(ndef_wifi(m, s33, "password1", NDEF_AUTH_WPA2) == 0, "Wi-Fi: network name of 33 bytes refused");
    CK(ndef_wifi(m, "n", k64, NDEF_AUTH_WPA2) == 0, "Wi-Fi: key of 64 characters refused"); }
  CK(ndef_wifi(m, "n", "short", NDEF_AUTH_WPA2) == 0 && ndef_wifi(m, "n", "1234567", NDEF_AUTH_WPA_WPA2) == 0, "Wi-Fi: key shorter than 8 refused");
  CK(ndef_wifi(m, "n", "12345678", NDEF_AUTH_OPEN) == 0, "Wi-Fi: open network with a key refused");
  CK(ndef_wifi(m, "", "12345678", NDEF_AUTH_WPA2) == 0 && ndef_wifi(m, NULL, "12345678", NDEF_AUTH_WPA2) == 0, "Wi-Fi: empty or missing name refused");
  CK(ndef_wifi(m, "n", "12345678", 3) == 0 && ndef_wifi(m, "n", "caf\xC3\xA9" "1234", NDEF_AUTH_WPA2) == 0, "Wi-Fi: bad auth code or non-ASCII key refused");
  CK(ndef_wifi(m, "caf\xC3\xA9", "12345678", NDEF_AUTH_WPA2) != 0, "Wi-Fi: a UTF-8 network name is allowed");
  // parser on other things
  CK(ndef_parse(m, 0, w, sizeof w) == NDEF_NONE && w[0] == 0, "empty message");
  { uint8_t t3[] = { 0xD1, 0x01, 0x04, 'X', 1, 2, 3, 4 }; CK(ndef_parse(t3, sizeof t3, w, sizeof w) == NDEF_OTHER, "other well-known type"); }
  { uint8_t mime[] = { 0xD2, 0x0A, 0x02, 't','e','x','t','/','p','l','a','i','n', 'h', 'i' }; CK(ndef_parse(mime, sizeof mime, w, sizeof w) == NDEF_OTHER, "other MIME type"); }
  { uint8_t ul[] = { 0xD1, 0x01, 0x04, 'U', 0x00, 'a', 'b', 'c' }; for (size_t k = 0; k < sizeof ul; k++) CK(ndef_parse(ul, k, w, sizeof w) == NDEF_NONE, "truncated message"); }
  { uint8_t lg[] = { 0xC1, 0x01, 0, 0, 0, 3, 'U', 0x00, 'a', 'b' }; CK(ndef_parse(lg, sizeof lg, w, sizeof w) == NDEF_URI && !strcmp(w, "ab"), "long record (SR clear)"); }
  { uint8_t id[] = { 0xD9, 0x01, 0x03, 0x02, 'U', 'i', 'd', 0x00, 'a', 'b' }; CK(ndef_parse(id, sizeof id, w, sizeof w) == NDEF_URI && !strcmp(w, "ab"), "record with an id field"); }
  { uint8_t tu[] = { 0xD1, 0x01, 0x07, 'T', 0x82, 'e', 'n', 'x', 'y', 'z', 'w' }; CK(ndef_parse(tu, sizeof tu, w, sizeof w) == NDEF_OTHER, "UTF-16 text is not shown"); }
  { uint8_t ct[] = { 0xD1, 0x01, 0x06, 'T', 0x02, 'e', 'n', 'a', 0x01, 'b' }; CK(ndef_parse(ct, sizeof ct, w, sizeof w) == NDEF_TEXT && !strcmp(w, "a?b"), "control characters in a text become ?"); }
  // a small output buffer is always terminated
  { char tiny[6]; n = ndef_uri(m, "https://example.org/path"); CK(ndef_parse(m, n, tiny, sizeof tiny) == NDEF_URI && strlen(tiny) == 5, "small output buffer is cut and terminated"); CK(ndef_parse(m, n, tiny, 1) == NDEF_URI && tiny[0] == 0, "output buffer of 1"); }
  // nested credential: bounded depth, no crash
  { uint8_t deep[NDEF_MAX]; size_t z = 0; deep[z++] = 0xD2; deep[z++] = 23; size_t pl = NDEF_MAX - 3 - 23; deep[z++] = (uint8_t)pl; memcpy(deep + z, "application/vnd.wfa.wsc", 23); z += 23;
    for (size_t k = 0; k + 4 <= pl; k += 4) { deep[z++] = 0x10; deep[z++] = 0x0E; deep[z++] = 0; deep[z++] = (uint8_t)(pl - k - 4); }
    while (z < NDEF_MAX) deep[z++] = 0;
    CK(ndef_parse(deep, NDEF_MAX, w, sizeof w) != NDEF_NONE || 1, "deeply nested credential does not crash"); }
  // random bytes and random builder inputs: no overrun (ASan), always terminated, builder output parses back
  int ok_rt = 0;
  for (int i = 0; i < 200000; i++) {
    uint8_t r[NDEF_MAX];
    size_t rl = rnd() % (NDEF_MAX + 1);
    for (size_t k = 0; k < rl; k++) r[k] = (rnd() % 3) ? (uint8_t)rnd() : (uint8_t)"\xD1\xD2\x01\x02U T"[rnd() % 6];
    size_t cap = 1 + rnd() % 130;
    char o[130]; memset(o, 'Z', sizeof o);
    ndef_parse(r, rl, o, cap);
    bool term = false; for (size_t k = 0; k < cap; k++) if (o[k] == 0) { term = true; break; }
    if (!term) { CK(0, "parser output not terminated"); break; }
    if (cap < sizeof o && o[cap] != 'Z') { CK(0, "parser wrote past the buffer"); break; }
  }
  for (int i = 0; i < 20000; i++) {
    char a[200], b[100]; size_t la = rnd() % 140, lb = rnd() % 70;
    for (size_t k = 0; k < la; k++) a[k] = (char)(32 + rnd() % 95); a[la] = 0;
    for (size_t k = 0; k < lb; k++) b[k] = (char)(32 + rnd() % 95); b[lb] = 0;
    uint8_t buf[NDEF_MAX + 64]; memset(buf, 0xEE, sizeof buf);
    size_t x = ndef_wifi(buf, a, b, (uint8_t)(rnd() % 3));
    if (buf[NDEF_MAX] != 0xEE && x == 0) { }
    bool fits = x == 0 || x <= NDEF_MAX;
    if (!fits) { CK(0, "Wi-Fi message larger than NDEF_MAX"); break; }
    for (size_t k = NDEF_MAX; k < sizeof buf; k++) if (buf[k] != 0xEE) { CK(0, "builder wrote past NDEF_MAX"); goto out; }
    if (x) { char o[NDEF_MAX]; if (ndef_parse(buf, x, o, sizeof o) != NDEF_WIFI || strcmp(o, a) != 0) { CK(0, "Wi-Fi round trip"); break; } ok_rt++; }
    x = ndef_text(buf, a);
    if (x) { char o[NDEF_MAX]; if (ndef_parse(buf, x, o, sizeof o) != NDEF_TEXT || strcmp(o, a) != 0) { CK(0, "text round trip"); break; } ok_rt++; }
    x = ndef_uri(buf, a);
    if (x) { char o[NDEF_MAX]; if (ndef_parse(buf, x, o, sizeof o) != NDEF_URI || strcmp(o, a) != 0) { CK(0, "URI round trip"); break; } ok_rt++; }
  }
out:
  printf("   random: 200000 byte strings parsed, %d builder round trips, no overrun\n", ok_rt);
  CK(ok_rt > 5000, "enough round trips happened");
}

// OpenDisplay MSD record: layout of the reference firmware (decode_msd_payload of py-opendisplay)
static void t_od(void)
{
  uint8_t b[16];
  od_msd_pack(b, 3000, 25, 5);
  CK(b[0] == 0x46 && b[1] == 0x24, "MSD company id 0x2446 little endian");
  int ok = 1;
  for (int i = 2; i <= 12; i++) if (b[i] != 0xFF) ok = 0;
  CK(ok, "MSD dynamic data is 0xFF");
  CK(b[13] == 130, "MSD temperature (25 C -> (25 + 40) * 2)");
  CK(b[14] == 0x2C && (b[15] & 1) == 1 && (b[15] >> 4) == 5, "MSD 3000 mV = 300 x 10 mV: low byte 0x2C, bit 8 in the status, loop counter 5");
  CK((((b[15] & 1) << 8) | b[14]) == 300, "MSD battery decodes to 3.00 V");
  CK((b[15] & 0x0E) == 0, "MSD flags reboot / connection / encryption are 0");
  od_msd_pack(b, 2995, -40, 0); CK((((b[15] & 1) << 8) | b[14]) == 300 && b[13] == 0, "MSD rounds to 10 mV, -40 C = 0");
  od_msd_pack(b, 2994, -60, 16); CK((((b[15] & 1) << 8) | b[14]) == 299 && b[13] == 0 && (b[15] >> 4) == 0, "MSD below range clamps, loop counter wraps at 16");
  od_msd_pack(b, 0, 120, 15); CK(b[14] == 0 && (b[15] & 1) == 0 && b[13] == 255 && (b[15] >> 4) == 15, "MSD 0 mV = no reading, hot clamps at 255");
  od_msd_pack(b, 65535, 0, 0); CK((((b[15] & 1) << 8) | b[14]) == 511, "MSD voltage clamps at 5.11 V");
  printf("   od: MSD record layout checked\n");
}

int main(int argc, char **argv)
{
  const char *w = argc > 1 ? argv[1] : "all";
  panel_test_select(0);
  bool all = !strcmp(w, "all");
  if (all || !strcmp(w, "lz")) t_lz();
  if (all || !strcmp(w, "set")) t_settings();
  if (all || !strcmp(w, "slot")) t_slots();
  if (!strcmp(w, "show")) t_show();
  if (all || !strcmp(w, "ota")) t_ota();
  if (all || !strcmp(w, "beacon")) t_beacon();
  if (all || !strcmp(w, "panel")) t_panel_all();
  if (all || !strcmp(w, "ndef")) t_ndef();
  if (all || !strcmp(w, "od")) t_od();
  checks += panel_test_checks(); fails += panel_test_fails();
  printf("%d checks, %d failed, vt_start_errors=%d\n", checks, fails, vt_start_errors);
  return fails ? 1 : 0;
}
