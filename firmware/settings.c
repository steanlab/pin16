// pin16: persistent settings, see settings.h. Own code.
#include <string.h>
#include "settings.h"
#include "ext_flash.h"

#define SET_A    0x7D000UL
#define SET_B    0x7E000UL
#define REC      32U
#define PER_SEC  (4096U / REC)

settings_t g_set;
static uint32_t seq = 0;
static int s_sec = 1;              // sector of the newest record (0 = A, 1 = B)
static uint32_t s_idx = PER_SEC;   // next free record in that sector; PER_SEC = full, the other sector is erased first

static uint32_t sec_addr(int s) { return s ? SET_B : SET_A; }

static void defaults(void)
{
  memset(&g_set, 0, sizeof(g_set));
  g_set.ss_en = 0; g_set.ss_every = 60; g_set.ss_mask = 0;
  g_set.orient = 0; g_set.od_slot = 31; g_set.lut = 0xE4; g_set.txp = 3; g_set.led = 2;
  g_set.opmode = 0; g_set.wt = 60; g_set.wb = 120; g_set.tap_next = 0;
  g_set.bt_en = 0; g_set.bt_itv = 2; g_set.bt_mask = 0x7F; g_set.splash_off = 0; g_set.bt_win = 0; g_set.ss_order = 0;
  g_set.amber_g = 150; g_set.bc_preset = 0; g_set.bc_col = 1; g_set.bc_lvl = 3; g_set.bc_dur = 1; g_set.bc_keep = 0;
}

static uint8_t chk(const uint8_t *r) { uint8_t c = 0x5A; for (int i = 0; i < 31; i++) c ^= r[i]; return c; }
// A checksum of 0xFF is never written (see settings_save) and never accepted: a record torn by a power cut still has its unprogrammed tail 0xFF.
static bool rec_ok(const uint8_t *r) { return r[0] == 'S' && (r[1] == '1' || r[1] == '2' || r[1] == '3') && r[31] != 0xFF && r[31] == chk(r); }
static uint32_t rec_seq(const uint8_t *r)
{
  uint32_t q = (uint32_t)r[2] | ((uint32_t)r[3] << 8) | ((uint32_t)r[4] << 16);
  return r[1] == '1' ? (q | ((uint32_t)r[5] << 24)) : q;      // S2: byte 5 is the amber setting
}

static void pack(uint8_t *r)
{
  memset(r, 0, REC);
  r[0] = 'S'; r[1] = '3';                         // 'S1' and 'S2' records are still read; S3 = the orientation is relative to the panel profile (v19)
  r[2] = (uint8_t)seq; r[3] = (uint8_t)(seq >> 8); r[4] = (uint8_t)(seq >> 16); r[5] = g_set.amber_g;
  r[6] = g_set.ss_en; r[7] = (uint8_t)g_set.ss_every; r[8] = (uint8_t)(g_set.ss_every >> 8);
  for (int i = 0; i < 4; i++) r[9 + i] = (uint8_t)(g_set.ss_mask >> (8 * i));
  r[13] = (uint8_t)((g_set.orient & 3) | ((g_set.bc_preset & 7) << 2) | ((g_set.bc_col & 7) << 5)); r[14] = g_set.od_slot; r[15] = g_set.lut; r[16] = g_set.txp; r[17] = g_set.led;
  r[18] = g_set.opmode; r[19] = g_set.wt; r[20] = g_set.wb; r[21] = g_set.tap_next;
  r[22] = g_set.clean_n; r[23] = (uint8_t)((g_set.clean_mode & 3) | (g_set.bc_keep ? 4 : 0) | (((g_set.bc_lvl ? g_set.bc_lvl - 1 : 0) & 3) << 3));
  memcpy(r + 24, g_set.name, 5);                  // the name has 5 characters at most: byte 29 is free
  r[29] = (uint8_t)((g_set.bt_mask & 0x7F) | (g_set.bt_en ? 0x80 : 0));
  r[30] = (uint8_t)((g_set.bt_itv & 3) | (g_set.splash_off ? 4 : 0) | (g_set.bt_win ? 8 : 0) | (g_set.ss_order ? 16 : 0) | ((g_set.bc_dur & 7) << 5));
  r[31] = chk(r);
}

static bool unpack(const uint8_t *r)
{
  if (!rec_ok(r)) return false;
  bool v2 = r[1] >= '2';
  seq = rec_seq(r);
  g_set.ss_en = r[6] ? 1 : 0;
  g_set.ss_every = (uint16_t)(r[7] | (r[8] << 8));
  g_set.ss_mask = 0;
  for (int i = 0; i < 4; i++) g_set.ss_mask |= (uint32_t)r[9 + i] << (8 * i);
  g_set.orient = (r[1] == '3') ? (r[13] & 3) : 0;     // older records: the saved orientation compensated the old picture, drop it
  g_set.od_slot = r[14]; g_set.lut = r[15]; g_set.txp = r[16]; g_set.led = r[17];
  g_set.opmode = r[18] ? 1 : 0; g_set.wt = r[19]; g_set.wb = r[20]; g_set.tap_next = r[21] ? 1 : 0;
  g_set.clean_n = r[22]; g_set.clean_mode = r[23];
  memcpy(g_set.name, r + 24, 5);
  g_set.name[5] = 0;
  g_set.bt_en = (r[29] & 0x80) ? 1 : 0;
  g_set.bt_mask = r[29] & 0x7F;                   // old records have 0 here: the beacon stays off
  if (g_set.bt_mask == 0) g_set.bt_mask = 0x7F;
  g_set.bt_itv = r[30] & 3;
  g_set.splash_off = (r[30] & 4) ? 1 : 0;
  g_set.bt_win = (r[30] & 8) ? 1 : 0;
  g_set.ss_order = (r[30] & 16) ? 1 : 0;
  g_set.amber_g = 150; g_set.bc_preset = 0; g_set.bc_col = 1; g_set.bc_lvl = 3; g_set.bc_dur = 1; g_set.bc_keep = 0;
  if (v2) {
    g_set.amber_g = r[5];
    g_set.bc_preset = (r[13] >> 2) & 7; g_set.bc_col = (r[13] >> 5) & 7;
    g_set.bc_keep = (r[23] & 4) ? 1 : 0; g_set.bc_lvl = (uint8_t)(((r[23] >> 3) & 3) + 1);
    g_set.bc_dur = (r[30] >> 5) & 7;
    g_set.clean_mode = r[23] & 3;
  }
  if (g_set.clean_mode > 2) g_set.clean_mode = 0;
  if (g_set.bc_preset > 5) g_set.bc_preset = 0;
  if (g_set.bc_col < 1 || g_set.bc_col > 7) g_set.bc_col = 1;
  if (g_set.bc_lvl < 1 || g_set.bc_lvl > 4) g_set.bc_lvl = 3;
  if (g_set.bc_dur > 4) g_set.bc_dur = 1;
  // limits
  if (g_set.ss_every < 1) g_set.ss_every = 1;
  if (g_set.ss_every > 1080) g_set.ss_every = 1080;
  if (g_set.od_slot > 31) g_set.od_slot = 31;
  if (g_set.txp > 5) g_set.txp = 3;
  if (g_set.led > 3) g_set.led = 3;
  if (g_set.wt < 10 || g_set.wt > 250) g_set.wt = 60;
  if (g_set.wb < 10 || g_set.wb > 250) g_set.wb = 120;
  for (int i = 0; i < 5; i++) if (g_set.name[i] && (g_set.name[i] < 0x21 || g_set.name[i] > 0x7E)) g_set.name[i] = 0;
  return true;
}

void settings_load(void)
{
  uint8_t id[3];
  defaults();
  seq = 0; s_sec = 1; s_idx = PER_SEC;
  if (!xf_begin(id)) return;
  bool found = false;
  uint32_t best_seq = 0, best_blank = PER_SEC;
  int best_sec = 0;
  uint8_t best_rec[REC];
  for (int s = 0; s < 2; s++) {
    uint32_t blank = PER_SEC;
    bool has = false;
    uint32_t sec_seq = 0;
    uint8_t sec_rec[REC];
    for (uint32_t i = 0; i < PER_SEC; i++) {
      uint8_t r[REC];
      xf_read(sec_addr(s) + i * REC, r, REC);
      bool all_ff = true;
      for (int k = 0; k < (int)REC; k++) if (r[k] != 0xFF) { all_ff = false; break; }
      if (all_ff) { blank = i; break; }
      if (rec_ok(r)) {
        uint32_t q = rec_seq(r);
        if (!has || q >= sec_seq) { has = true; sec_seq = q; memcpy(sec_rec, r, REC); }
      }
    }
    if (has && (!found || sec_seq > best_seq)) {
      found = true; best_seq = sec_seq; best_sec = s; best_blank = blank; memcpy(best_rec, sec_rec, REC);
    }
  }
  xf_end();
  if (found && unpack(best_rec)) {
    s_sec = best_sec;
    s_idx = best_blank;
  } else {
    defaults();
    seq = 0;
  }
}

bool settings_save(void)
{
  uint8_t id[3];
  if (!xf_begin(id)) return false;
  if (s_idx >= PER_SEC) {
    int ns = s_sec ^ 1;
    if (!xf_erase_sector(sec_addr(ns))) { xf_end(); return false; }   // stays "full": the next save tries the erase again
    s_sec = ns;
    s_idx = 0;
  }
  uint32_t seq0 = seq;
  uint8_t r[REC];
  seq++;
  pack(r);
  for (int k = 0; k < 16 && r[31] == 0xFF; k++) { seq++; pack(r); }      // skip a sequence number rather than write a checksum of 0xFF
  bool ok = xf_write(sec_addr(s_sec) + s_idx * REC, r, REC);
  s_idx++;                                        // a record that failed to program is skipped, never reused
  if (!ok) seq = seq0;
  xf_end();
  return ok;
}

uint32_t settings_seq(void) { return seq; }
