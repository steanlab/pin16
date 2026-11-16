// pin16: picture slots and upload protocol, see slot.h. Own code.
#include <stdio.h>
#include <string.h>
#include "slot.h"
#include "ext_flash.h"
#include "ble_badge.h"
#include "lz.h"
#include "settings.h"
#include "pics.h"
#include "draw.h"
#include "sl_sleeptimer.h"

typedef struct { uint8_t used, fmt, nsect; uint16_t first; uint32_t len, crc, serial; } dir_t;
static dir_t dir[SLOT_N];
static bool dir_ok = false;
static uint32_t next_serial = 1;
static uint32_t slot_announced_crc;     // CRC-32 the client announced with command 42

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static uint32_t crc_upd(uint32_t c, const uint8_t *p, uint32_t n)       // CRC-32 (zlib), nibble table
{
  static const uint32_t t[16] = { 0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
                                  0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C };
  while (n--) {
    c ^= *p++;
    c = (c >> 4) ^ t[c & 15];
    c = (c >> 4) ^ t[c & 15];
  }
  return c;
}

static uint32_t sect_addr(int i) { return SLOT_BEG + (uint32_t)i * SLOT_SECT; }
static uint32_t nsect_for(uint32_t len) { return (SLOT_HDR + len + SLOT_SECT - 1) / SLOT_SECT; }

static bool fl_open(void)
{
  uint8_t id[3];
  return xf_begin(id);
}

// ---- directory ------------------------------------------------------------
static void scan(void)                  // flash must be open
{
  memset(dir, 0, sizeof(dir));
  next_serial = 1;
  for (uint32_t i = 0; i < SLOT_NSECT;) {
    uint8_t h[20];
    xf_read(sect_addr(i), h, sizeof(h));
    uint32_t ns = h[7];
    if (memcmp(h, "BG22", 4) == 0 && h[4] == 1 && h[6] < SLOT_N && h[5] <= 2 && ns >= 1 && ns <= 3 && i + ns <= SLOT_NSECT
        && nsect_for(get32(h + 8)) == ns) {
      uint32_t ser = get32(h + 16);
      dir_t *d = &dir[h[6]];
      if (!d->used || ser > d->serial) {
        d->used = 1; d->fmt = h[5]; d->nsect = (uint8_t)ns; d->first = (uint16_t)i;
        d->len = get32(h + 8); d->crc = get32(h + 12); d->serial = ser;
      }
      if (ser >= next_serial) next_serial = ser + 1;
      i += ns;
    } else {
      i++;
    }
  }
  dir_ok = true;
}

static bool need_dir(void)
{
  if (dir_ok) return true;
  if (!fl_open()) return false;
  scan();
  xf_end();
  return true;
}

static bool sect_used(int i)
{
  for (int s = 0; s < SLOT_N; s++) if (dir[s].used && i >= dir[s].first && i < dir[s].first + dir[s].nsect) return true;
  return false;
}

static void free_stats(int *freen, int *largest)
{
  int f = 0, run = 0, best = 0;
  for (int i = 0; i < (int)SLOT_NSECT; i++) {
    if (sect_used(i)) { run = 0; } else { f++; run++; if (run > best) best = run; }
  }
  *freen = f; *largest = best;
}

static int find_run(int n)
{
  int run = 0;
  for (int i = 0; i < (int)SLOT_NSECT; i++) {
    if (sect_used(i)) { run = 0; } else { run++; if (run == n) return i - n + 1; }
  }
  return -1;
}

static bool sector_blank(uint32_t a)    // flash open
{
  uint8_t b[64];
  for (uint32_t o = 0; o < SLOT_SECT; o += sizeof(b)) {
    xf_read(a + o, b, sizeof(b));
    for (uint32_t k = 0; k < sizeof(b); k++) if (b[k] != 0xFF) return false;
  }
  return true;
}

static bool erase_run(int first, int n, bool only_dirty)   // flash open
{
  for (int i = first; i < first + n; i++) {
    uint32_t a = sect_addr(i);
    if (only_dirty && sector_blank(a)) continue;
    if (!xf_erase_sector(a)) return false;
  }
  return true;
}

// ---- upload ---------------------------------------------------------------
enum { UP_IDLE, UP_RUN };
static uint8_t  up_state = UP_IDLE;
static uint8_t  up_slot, up_fmt, up_win, up_nsect;
static uint16_t up_first, up_seq, up_inwin;
static uint32_t up_len, up_got, up_crc, up_fill;
static uint8_t  up_page[256];
static bool     up_check;               // compare with the CRC announced by command 42

static void up_close(void)
{
  if (up_state != UP_IDLE) {
    up_state = UP_IDLE;
    xf_end();
  }
}

// Starts an upload: erases the old picture of the slot, reserves and erases a run. NULL = ready, else the reason.
static const char *up_start(uint8_t ss, uint32_t l, uint8_t fmt, uint8_t win, bool check_crc)
{
  if (up_state != UP_IDLE) up_close();
  if (ss >= SLOT_N) return "slot";
  if (!panel_ok()) return "panel";
  if (fmt > 2) return "fmt";
  if (win < 1 || win > 32) win = 8;
  if (fmt == 0 ? l != FRAME_BYTES : (l < 2 || l > SLOT_MAXLEN)) return "len";
  if (!fl_open()) return "flash";
  if (!dir_ok) scan();
  up_state = UP_RUN;
  if (dir[ss].used) {                         // the old picture goes first
    int f = dir[ss].first, n = dir[ss].nsect;
    dir[ss].used = 0;
    if (!erase_run(f, n, false)) { up_close(); return "erase"; }
  }
  int ns = (int)nsect_for(l);
  int first = find_run(ns);
  if (first < 0) {
    int fr, lg;
    free_stats(&fr, &lg);
    ble_badge_say("slot %u: no room for %d sectors (free %d, largest run %d)", ss, ns, fr, lg);
    up_close();
    return "room";
  }
  if (!erase_run(first, ns, true)) { up_close(); return "erase"; }
  up_slot = ss; up_fmt = fmt; up_win = win; up_nsect = (uint8_t)ns; up_first = (uint16_t)first;
  up_len = l; up_got = 0; up_crc = 0xFFFFFFFFUL; up_fill = 0; up_seq = 0; up_inwin = 0; up_check = check_crc;
  return NULL;
}

static bool flush_page(void)
{
  if (!up_fill) return true;
  uint32_t pos = up_got - up_fill;            // stored-stream offset of the first buffered byte
  bool ok = xf_write(sect_addr(up_first) + SLOT_HDR + pos, up_page, up_fill);
  up_fill = 0;
  return ok;
}

// Adds bytes to the running upload. NULL = ok, else the reason (the upload is closed).
static const char *up_append(const uint8_t *d, uint32_t n)
{
  if (up_state != UP_RUN) return "no upload";
  if (up_got + n > up_len) { up_close(); return "too long"; }
  up_crc = crc_upd(up_crc, d, n);
  for (uint32_t i = 0; i < n; i++) {
    up_page[up_fill++] = d[i];
    up_got++;
    if (up_fill == sizeof(up_page) && !flush_page()) { up_close(); return "write"; }
  }
  return NULL;
}

static uint32_t flash_crc(uint32_t addr, uint32_t len)   // flash open
{
  uint8_t b[128];
  uint32_t c = 0xFFFFFFFFUL;
  while (len) {
    uint32_t n = len > sizeof(b) ? sizeof(b) : len;
    xf_read(addr, b, n);
    c = crc_upd(c, b, n);
    addr += n; len -= n;
  }
  return ~c;
}

// Finishes the upload: checks, header last. NULL = ok (slot number and CRC in the outputs), else the reason.
static const char *up_finish(uint8_t *slot_out, uint32_t *crc_out)
{
  if (up_state != UP_RUN) return "no upload";
  const char *e = NULL;
  uint32_t crc = ~up_crc;
  uint32_t base = sect_addr(up_first) + SLOT_HDR;
  if (up_got != up_len) e = "incomplete";
  else if (!flush_page()) e = "write";
  else if (flash_crc(base, up_len) != crc) e = "readback";
  else if (up_check && crc != slot_announced_crc) e = "crc";
  else if (up_fmt && !lz_check(base, up_len, up_fmt == 2, panel_line_bytes(), panel_h())) e = "decode";
  if (!e) {
    uint8_t h[SLOT_HDR];
    memset(h, 0xFF, sizeof(h));
    memcpy(h, "BG22", 4);
    h[4] = 1; h[5] = up_fmt; h[6] = up_slot; h[7] = up_nsect;
    put32(h + 8, up_len); put32(h + 12, crc); put32(h + 16, next_serial);
    if (!xf_write(sect_addr(up_first), h, sizeof(h))) e = "header";
  }
  if (!e) {
    dir_t *s = &dir[up_slot];
    s->used = 1; s->fmt = up_fmt; s->nsect = up_nsect; s->first = up_first; s->len = up_len; s->crc = crc; s->serial = next_serial++;
    *slot_out = up_slot;
    *crc_out = crc;
  }
  up_close();
  return e;
}

static void up_begin(const uint8_t *d, uint8_t len)
{
  if (len < 11) { ble_badge_say("UP FAIL short"); return; }
  uint8_t fmt = len > 11 ? d[11] : 0;
  const char *e = up_start(d[1], get32(d + 2), fmt, d[10], true);
  if (e) { if (strcmp(e, "room")) ble_badge_say("UP FAIL %s", e); return; }
  uint16_t mtu = ble_badge_mtu();
  uint16_t chunk = mtu > 6 ? (uint16_t)(mtu - 6) : 1;
  if (chunk > 238) chunk = 238;
  ble_badge_say("UP READY chunk=%u win=%u mtu=%u rx=244 fmt=%u sect=%u", chunk, up_win, mtu, fmt, up_nsect);
}

static void up_data(const uint8_t *d, uint8_t len)
{
  if (up_state != UP_RUN) { ble_badge_say("UP FAIL no upload"); return; }
  if (len < 3) { up_close(); ble_badge_say("UP FAIL short"); return; }
  uint16_t seq = (uint16_t)(d[1] | (d[2] << 8));
  if (seq != up_seq) {
    uint16_t want = up_seq;
    up_close();
    ble_badge_say("UP FAIL seq %u expected %u", seq, want);
    return;
  }
  const char *e = up_append(d + 3, (uint32_t)len - 3);
  if (e) { ble_badge_say("UP FAIL %s", e); return; }
  up_seq++;
  up_inwin++;
  if (up_inwin >= up_win || up_got == up_len) {
    up_inwin = 0;
    ble_badge_say("UP ACK %lu", (unsigned long)up_got);
  }
}

static void up_end(void)
{
  uint8_t sl; uint32_t crc;
  const char *e = up_finish(&sl, &crc);
  if (e) { ble_badge_say("UP FAIL %s", e); return; }
  ble_badge_say("UP OK slot %u crc=%08lX", sl, (unsigned long)crc);
}

// ---- OpenDisplay direct write (used by od.c) ------------------------------
bool slot_od_begin(void)
{
  if (up_state != UP_IDLE) return false;
  return up_start(g_set.od_slot, FRAME_BYTES, 0, 8, false) == NULL;
}
bool slot_od_data(const uint8_t *d, uint32_t n) { return up_append(d, n) == NULL; }
bool slot_od_end(void)
{
  uint8_t sl; uint32_t crc;
  return up_finish(&sl, &crc) == NULL;
}

// ---- drawing and slideshow --------------------------------------------------
static int cur_slot = -1;
static uint8_t pic_req = 0;                     // picture of pics.c to draw from the main loop, 0 = none
static bool pic_boot = false;                   // open the power-up window after it
static uint8_t clean_idx = 0xFF;                // clean cycle running: step, 0xFF = not running
static const uint8_t seq_full[] = { PIC_BLACK, PIC_WHITE, PIC_RED, PIC_YELLOW, PIC_WHITE };
static const uint8_t seq_w[]    = { PIC_WHITE };
static const uint8_t seq_bw[]   = { PIC_BLACK, PIC_WHITE };
static const uint8_t *clean_seq = seq_full;
static uint8_t clean_len = sizeof(seq_full);
static int resume_slot = -1;                    // slide to draw when the clean sequence has finished
static bool draw_from_show = false;             // the pending draw comes from the slideshow timer (counts for the periodic clean)
static uint8_t since_clean = 0;
static int draw_req = -1;                       // slot to draw from the main loop, 0xFF = none to draw, -1 nothing requested
static bool draw_od = false;                    // the draw was asked by an OpenDisplay client: answer 00 73 / 00 74
static volatile bool show_flag = false;
static sl_sleeptimer_timer_handle_t show_timer;
static bool show_armed = false;
static bool return_after_show = false;          // the slideshow step was postponed (radio busy)
static bool win_after_draw = false;             // tap: open the Bluetooth window only after the draw is finished

static bool in_mask(int s) { return g_set.ss_mask == 0 || (g_set.ss_mask >> s) & 1; }

static uint32_t rnd_state = 0;
static uint32_t rnd(void)
{
  if (rnd_state == 0) rnd_state = sl_sleeptimer_get_tick_count() | 1u;
  rnd_state ^= rnd_state << 13; rnd_state ^= rnd_state >> 17; rnd_state ^= rnd_state << 5;
  return rnd_state;
}

static int next_slot(bool use_mask)
{
  if (use_mask && g_set.ss_order == 1) {                // random order: any slot of the show except the one on the panel (if there is another)
    int cand[SLOT_N], n = 0;
    for (int s = 0; s < SLOT_N; s++) if (dir[s].used && in_mask(s) && s != cur_slot) cand[n++] = s;
    if (n == 0 && cur_slot >= 0 && dir[cur_slot].used && in_mask(cur_slot)) return cur_slot;
    if (n == 0) return -1;
    return cand[rnd() % (uint32_t)n];
  }
  for (int k = 1; k <= SLOT_N; k++) {
    int s = (cur_slot + k + SLOT_N) % SLOT_N;
    if (dir[s].used && (!use_mask || in_mask(s))) return s;
  }
  return -1;
}

static void on_show_timer(sl_sleeptimer_timer_handle_t *h, void *d) { (void)h; (void)d; show_flag = true; }

void slot_show_arm(void)                        // (re)start the slideshow timer from the settings
{
  if (show_armed) { sl_sleeptimer_stop_timer(&show_timer); show_armed = false; }
  if (g_set.ss_en) {
    sl_sleeptimer_start_timer_ms(&show_timer, (uint32_t)g_set.ss_every * 60000UL, on_show_timer, NULL, 0, 0);
    show_armed = true;
  }
}

void slot_request_next(void)                    // from the tap code or command 4C
{
  if (need_dir()) {
    int n = next_slot(false);
    draw_req = n < 0 ? 0xFF : n;
  }
}

void slot_request_next_tap(void)                // tap on the tag: draw first, open the Bluetooth window afterwards
{
  slot_request_next();
  if (draw_req == -1) ble_badge_request_window(); else win_after_draw = true;
}

void slot_request_draw(int s, bool od)
{
  if (need_dir()) { draw_req = (s >= 0 && s < SLOT_N) ? s : 0xFF; draw_od = od; }
}

bool slot_request_pic(uint8_t kind, bool boot)
{
  if (!pic_valid(kind) || pic_req != 0 || draw_req != -1 || clean_idx != 0xFF) return false;
  pic_req = kind;
  pic_boot = boot;
  return true;
}

bool slot_request_clean(void)
{
  if (pic_req != 0 || draw_req != -1 || clean_idx != 0xFF) return false;
  clean_seq = seq_full; clean_len = sizeof(seq_full);
  resume_slot = -1;
  clean_idx = 0;
  pic_req = clean_seq[0];
  pic_boot = false;
  return true;
}

static void clean_then(int slot, uint8_t mode)           // periodic clean before a slideshow slide: 1 white, 2 black then white
{
  if (mode == 2) { clean_seq = seq_bw; clean_len = sizeof(seq_bw); } else { clean_seq = seq_w; clean_len = sizeof(seq_w); }
  resume_slot = slot;
  clean_idx = 0;
  pic_req = clean_seq[0];
  pic_boot = false;
}

// ---- slot read-back for thumbnails ----
static int dump_slot = -1;
static uint32_t dump_addr, dump_len, dump_off;
static uint16_t dump_seq;

bool slot_dump_active(void) { return dump_slot >= 0; }

bool slot_dump_start(int s)
{
  if (up_state != UP_IDLE || dump_slot >= 0 || !need_dir() || s < 0 || s >= SLOT_N || !dir[s].used) return false;
  dump_slot = s;
  dump_addr = sect_addr(dir[s].first) + SLOT_HDR;
  dump_len = dir[s].len;
  dump_off = 0;
  dump_seq = 0;
  ble_badge_say("DUMP %d fmt %u len %lu crc=%08lX", s, dir[s].fmt, (unsigned long)dump_len, (unsigned long)dir[s].crc);
  return true;
}

static void dump_step(void)
{
  int guard = 0;
  while (dump_slot >= 0 && ble_badge_room() && guard++ < 8) {
    if (!ble_badge_link_ready()) { dump_slot = -1; return; }
    if (dump_off >= dump_len) {
      ble_badge_say("DUMP done %d", dump_slot);
      dump_slot = -1;
      return;
    }
    uint32_t cap = ble_badge_mtu() > 3 ? (uint32_t)ble_badge_mtu() - 3 : 20;
    cap = cap > 4 + 90 ? 90 : (cap > 12 ? cap - 4 : 8);
    uint32_t n = dump_len - dump_off;
    if (n > cap) n = cap;
    uint8_t f[4 + 90];
    if (!fl_open()) { ble_badge_say("DUMP fail %d", dump_slot); dump_slot = -1; return; }
    xf_read(dump_addr + dump_off, f + 4, n);
    xf_end();
    f[0] = 0xC0; f[1] = (uint8_t)dump_slot; f[2] = (uint8_t)dump_seq; f[3] = (uint8_t)(dump_seq >> 8);
    ble_badge_send(f, (uint8_t)(4 + n));
    dump_off += n;
    dump_seq++;
  }
  ble_badge_touch();
}

void slot_poll(void)                            // main loop: runs a requested draw or a slideshow step
{
  if (dump_slot >= 0) {
    dump_step();
    return;
  }
  if (show_flag) {
    show_flag = false;
    show_armed = false;
    if (g_set.opmode == 0 && ble_badge_is_active()) {                // a draw blocks the radio for ~25 s: wait until the Bluetooth window and link are closed
      if (g_set.ss_en) {
        sl_sleeptimer_start_timer_ms(&show_timer, 5000UL, on_show_timer, NULL, 0, 0);
        show_armed = true;
      }
      return_after_show = true;
    }
    if (!return_after_show && g_set.ss_en && draw_req == -1 && up_state == UP_IDLE && need_dir()) {
      int n = next_slot(true);
      if (n >= 0) { draw_req = n; draw_from_show = true; }
    }
    if (!return_after_show) slot_show_arm();
    return_after_show = false;
  }
  if (pic_req != 0) {
    uint8_t k = pic_req;
    bool boot = pic_boot;
    pic_req = 0;
    pic_boot = false;
    ble_badge_say("PIC %u start", k);
    bool quiet = resume_slot >= 0;                              // a periodic clean inside the slideshow: no LED
    if (!quiet) ble_badge_led_work(0);
    bool ok = draw_pic(k, g_set.orient);
    ble_badge_touch();
    if (!quiet && !(clean_idx != 0xFF && ok && (size_t)(clean_idx + 1) < clean_len)) ble_badge_led_work((boot && ok) ? 3 : (ok ? 1 : 2));
    ble_badge_say(ok ? "PIC %u done" : "PIC %u fail", k);
    if (clean_idx != 0xFF) {
      if (ok && (size_t)(clean_idx + 1) < clean_len) {
        clean_idx++;
        pic_req = clean_seq[clean_idx];
      } else {
        clean_idx = 0xFF;
        ble_badge_say(ok ? "CLEAN done" : "CLEAN stopped");
        if (ok && resume_slot >= 0) { draw_req = resume_slot; draw_od = false; }
        resume_slot = -1;
      }
    }
    if (boot) ble_badge_request_boot_window();
    return;
  }
  if (draw_req == -1) return;
  int s = draw_req;
  bool od = draw_od;
  bool from_show = draw_from_show;
  draw_req = -1;
  draw_od = false;
  draw_from_show = false;
  if (s == 0xFF || s >= SLOT_N || !dir[s].used) {
    if (od) { uint8_t r[2] = { 0x00, 0x74 }; ble_badge_send(r, 2); } else ble_badge_say("DRAW no slot");
    if (win_after_draw) { win_after_draw = false; ble_badge_request_window(); }
    return;
  }
  if (from_show && g_set.clean_n && g_set.clean_mode) {            // periodic full redraw before every n-th slideshow slide
    if (++since_clean >= g_set.clean_n) {
      since_clean = 0;
      ble_badge_say("CLEAN before slot %d", s);
      clean_then(s, g_set.clean_mode);
      return;
    }
  }
  uint32_t a = sect_addr(dir[s].first) + SLOT_HDR;
  ble_badge_say("DRAW slot %d", s);
  if (!from_show) ble_badge_led_work(0);                        // the slideshow changes slides silently
  bool ok = draw_slot(a, dir[s].len, dir[s].fmt, g_set.orient, g_set.lut);
  cur_slot = s;
  ble_badge_touch();
  if (!from_show) ble_badge_led_work(ok ? 1 : 2);
  if (od) { uint8_t r[2] = { 0x00, (uint8_t)(ok ? 0x73 : 0x74) }; ble_badge_send(r, 2); }
  else ble_badge_say(ok ? "DRAW done" : "DRAW fail");
  if (win_after_draw) { win_after_draw = false; ble_badge_request_window(); }
}

bool slot_draw_pending(void) { return draw_req != -1 || show_flag || pic_req != 0 || dump_slot >= 0; }

// ---- other commands -------------------------------------------------------
static void list_slots(void)
{
  if (!need_dir()) { ble_badge_say("ERR flash"); return; }
  int used = 0;
  for (int s = 0; s < SLOT_N; s++) {
    if (!dir[s].used) continue;
    used++;
    ble_badge_say("slot %d: fmt %u, %lu bytes, %u sectors, crc=%08lX", s, dir[s].fmt, (unsigned long)dir[s].len, dir[s].nsect, (unsigned long)dir[s].crc);
  }
  int fr, lg;
  free_stats(&fr, &lg);
  ble_badge_say("slots: %d used, %d of %d sectors free, largest run %d, max %d slots", used, fr, (int)SLOT_NSECT, lg, SLOT_N);
}

static void erase_slot(uint8_t ss)
{
  if (up_state != UP_IDLE) { ble_badge_say("ERR busy"); return; }
  if (!fl_open()) { ble_badge_say("ERR flash"); return; }
  if (!dir_ok) scan();
  if (ss == 0xFF) {
    erase_run(0, (int)SLOT_NSECT, true);
    memset(dir, 0, sizeof(dir));
    ble_badge_say("ERASED all");
  } else if (ss < SLOT_N) {
    if (dir[ss].used) {
      erase_run(dir[ss].first, dir[ss].nsect, false);
      dir[ss].used = 0;
    }
    ble_badge_say("ERASED slot %u", ss);
  } else {
    ble_badge_say("ERR slot");
  }
  xf_end();
}

static void verify_slot(uint8_t ss)
{
  if (up_state != UP_IDLE) { ble_badge_say("ERR busy"); return; }
  if (!need_dir()) { ble_badge_say("ERR flash"); return; }
  if (ss >= SLOT_N || !dir[ss].used) { ble_badge_say("VERIFY slot %u empty", ss); return; }
  if (!fl_open()) { ble_badge_say("ERR flash"); return; }
  uint32_t base = sect_addr(dir[ss].first) + SLOT_HDR;
  uint32_t c = flash_crc(base, dir[ss].len);
  if (c != dir[ss].crc) {
    ble_badge_say("VERIFY slot %u FAIL crc=%08lX", ss, (unsigned long)c);
  } else if (dir[ss].fmt && !lz_check(base, dir[ss].len, dir[ss].fmt == 2, panel_line_bytes(), panel_h())) {
    ble_badge_say("VERIFY slot %u FAIL decode", ss);
  } else {
    ble_badge_say("VERIFY slot %u ok crc=%08lX", ss, (unsigned long)c);
  }
  xf_end();
}

bool slot_is_cmd(uint8_t c)
{
  return (c >= CMD_SLOT_LIST && c <= CMD_SLOT_ABORT) || c == CMD_SLOT_NEXT;
}
bool slot_busy(void) { return up_state != UP_IDLE; }
void slot_link_closed(void) { up_close(); dump_slot = -1; }

bool slot_valid(int s) { return s >= 0 && s < SLOT_N && dir[s].used; }

bool slot_info(int s, uint32_t *data_addr, uint32_t *len, uint8_t *fmt)
{
  if (!dir_ok || !slot_valid(s)) return false;
  *data_addr = sect_addr(dir[s].first) + SLOT_HDR;
  *len = dir[s].len;
  *fmt = dir[s].fmt;
  return true;
}

void slot_cmd(const uint8_t *d, uint8_t len)
{
  switch (d[0]) {
    case CMD_SLOT_LIST:   list_slots(); break;
    case CMD_SLOT_ERASE:  if (len < 2) ble_badge_say("ERR short"); else erase_slot(d[1]); break;
    case CMD_SLOT_UP:
      if (len >= 11) slot_announced_crc = get32(d + 6);
      up_begin(d, len);
      break;
    case CMD_SLOT_DRAW:
      if (len < 2) { ble_badge_say("ERR short"); break; }
      if (up_state != UP_IDLE) { ble_badge_say("ERR busy"); break; }
      slot_request_draw(d[1], false);
      break;
    case CMD_SLOT_NEXT:
      if (up_state != UP_IDLE) { ble_badge_say("ERR busy"); break; }
      slot_request_next();
      break;
    case CMD_SLOT_DATA:   up_data(d, len); break;
    case CMD_SLOT_END:    up_end(); break;
    case CMD_SLOT_VERIFY: if (len < 2) ble_badge_say("ERR short"); else verify_slot(d[1]); break;
    case CMD_SLOT_MTU:    ble_badge_say("MTU mtu=%u rx=244", ble_badge_mtu()); break;
    case CMD_SLOT_ABORT:
      if (up_state != UP_IDLE) { up_close(); ble_badge_say("UP ABORT"); } else { ble_badge_say("UP idle"); }
      break;
    default: ble_badge_say("ERR cmd %02X", d[0]); break;
  }
}

int slot_next_preview(void)             // for the status lines: the slot the slideshow would draw next, -1 none
{
  return (need_dir() && g_set.ss_order != 1) ? next_slot(true) : -1;
}
int slot_current(void) { return cur_slot; }
