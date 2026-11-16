// pin16: diagnostic build only (-DBG_DIAG): the "hardware card" of a tag, see docs/DIAG.md. Own code.
// Two parts, both read out over Bluetooth as text lines with command 4F 0D (or 4F 0F):
//   1. what the tag knows about itself: ids, factory data page, pin table, NFC chip answers, live pin levels
//   2. a trace of everything that went over the panel bus and the NFC bus after power-up: commands, data (first bytes and length),
//      reset pin, waits, BUSY waits, with the time. It fills once (2 KB) and then stops; 4F 0F 00 clears it, the next draw is recorded.
// The normal firmware has no trace code and no trace memory: hwcard.h turns the hooks into empty statements.
#ifdef BG_DIAG

#include <stdio.h>
#include <string.h>
#include "em_gpio.h"
#include "sl_sleeptimer.h"
#include "hw.h"
#include "nfc.h"
#include "panel.h"
#include "ext_flash.h"
#include "fwver.h"
#include "ble_badge.h"
#include "hwcard.h"

#define TR_BYTES 2048
#define TR_KEEP  48           // bytes kept of a record
#define TR_BIG   8            // bytes kept of a long data run (the pixel stream is only counted)
#define TR_HDR   12
#define TR_NFC_MAX 640        // NFC traffic may use only the first part of the buffer, the rest is for the panel sequence

typedef struct { char type; uint8_t nb; uint16_t blocks; uint32_t t, total; const uint8_t *d; } rec_t;

static uint8_t  buf[TR_BYTES];
static uint16_t tr_end = 0, tr_last = 0xFFFF;
static bool     tr_big = false, tr_full = false, tr_started = false;
static uint32_t tr_t0 = 0;

void hwc_clear(void)
{
  tr_end = 0; tr_last = 0xFFFF; tr_big = false; tr_full = false; tr_started = false;
}

static const hw_pin_t *pin_of(const hw_pin_t *p) { return p; }

static uint8_t pin_in(const hw_pin_t *p) { return (uint8_t)GPIO_PinInGet((GPIO_Port_TypeDef)pin_of(p)->port, pin_of(p)->pin); }

uint8_t hwc_levels(void)
{
  const hw_panel_t *p = &hw_pins.panel;
  uint8_t v = 0;
  if (pin_in(&p->busy)) v |= 1;
  if (pin_in(&p->rst))  v |= 2;
  if (pin_in(&p->dc))   v |= 4;
  return v;
}

void hwc_add(char type, const uint8_t *d0, size_t n0, const uint8_t *d1, size_t n1, uint32_t total)
{
  if (tr_full) return;
  if (type == 'I' && tr_end > TR_NFC_MAX) return;
  uint32_t now = sl_sleeptimer_tick_to_ms(sl_sleeptimer_get_tick_count());
  if (!tr_started) { tr_started = true; tr_t0 = now; }
  size_t nb = n0 + n1;
  bool big = false;
  if (type == 'D' && total > 16) { big = true; nb = TR_BIG < n1 ? TR_BIG : n1; n0 = 0; n1 = nb; }
  if (nb > TR_KEEP) { nb = TR_KEEP; if (n0 > nb) n0 = nb; n1 = nb - n0; }
  if (big && tr_big && tr_last != 0xFFFF && buf[tr_last] == 'D') {    // one more line of the same pixel stream: only count it
    uint32_t t; uint16_t b;
    memcpy(&t, &buf[tr_last + 8], 4); t += total; memcpy(&buf[tr_last + 8], &t, 4);
    memcpy(&b, &buf[tr_last + 2], 2); b++;        memcpy(&buf[tr_last + 2], &b, 2);
    return;
  }
  if (tr_end + TR_HDR + nb > TR_BYTES) { tr_full = true; return; }
  uint16_t at = tr_end, blocks = 1;
  uint32_t t = now - tr_t0;
  buf[at] = (uint8_t)type;
  buf[at + 1] = (uint8_t)nb;
  memcpy(&buf[at + 2], &blocks, 2);
  memcpy(&buf[at + 4], &t, 4);
  memcpy(&buf[at + 8], &total, 4);
  if (n0) memcpy(&buf[at + TR_HDR], d0, n0);
  if (n1) memcpy(&buf[at + TR_HDR + n0], d1, n1);
  tr_last = at;
  tr_big = big;
  tr_end = (uint16_t)(at + TR_HDR + nb);
}

// I2C exchange of the NFC chip: address, flags, result, lengths, then up to 12 bytes of each buffer
void hwc_i2c(uint8_t addr, uint16_t flags, int ok, const uint8_t *b0, size_t n0, const uint8_t *b1, size_t n1)
{
  uint8_t t[5 + 24];
  size_t k0 = n0 < 12 ? n0 : 12, k1 = n1 < 12 ? n1 : 12;
  t[0] = addr; t[1] = (uint8_t)flags; t[2] = (uint8_t)ok; t[3] = (uint8_t)n0; t[4] = (uint8_t)n1;
  if (k0 && b0) memcpy(&t[5], b0, k0); else k0 = 0;
  if (k1 && b1) memcpy(&t[5 + k0], b1, k1); else k1 = 0;
  hwc_add('I', t, 5, &t[5], k0 + k1, (uint32_t)(n0 | (n1 << 16)));
}

static bool next(uint16_t *off, rec_t *r)
{
  if (*off + TR_HDR > tr_end) return false;
  const uint8_t *p = &buf[*off];
  r->type = (char)p[0];
  r->nb = p[1];
  memcpy(&r->blocks, p + 2, 2);
  memcpy(&r->t, p + 4, 4);
  memcpy(&r->total, p + 8, 4);
  r->d = p + TR_HDR;
  *off = (uint16_t)(*off + TR_HDR + r->nb);
  return true;
}

// ---- the card: one text line per call, false after the last ----

static void pn(char *b, size_t n, int *u, const char *name, const hw_pin_t *p)
{
  if (p->port == HW_NONE) *u += snprintf(b + *u, n - (size_t)*u, "%s=- ", name);
  else *u += snprintf(b + *u, n - (size_t)*u, "%s=P%c%u ", name, 'A' + p->port, (unsigned)p->pin);
}

#define CARD_LINES 24

static bool card_line(int part, char *b, size_t n)
{
  const hw_userdata_t *ud = hw_userdata();
  const hw_tag_t *t = &hw_pins;
  const nfc_info_t *ni = nfc_info();
  int u = 0;
  b[0] = 0;
  switch (part) {
    case 0:
      snprintf(b, n, "HW tag=%02X ctrl=%02X col=%02X res=%ux%u capa=%02X%02X", ud->tag, ud->ctrl, ud->color, (unsigned)ud->xres, (unsigned)ud->yres, ud->capa0, ud->capa1);
      return true;
    case 1:
      snprintf(b, n, "PANEL %s ok=%u %ux%u mirror=%u", panel_name(), (unsigned)panel_ok(), (unsigned)panel_w(), (unsigned)panel_h(), (unsigned)panel_mirror());
      return true;
    case 2:
      pn(b, n, &u, "SCK", &t->panel.sck); pn(b, n, &u, "MOSI", &t->panel.mosi); pn(b, n, &u, "CS", &t->panel.cs);
      pn(b, n, &u, "DC", &t->panel.dc); pn(b, n, &u, "RST", &t->panel.rst);
      return true;
    case 3:
      pn(b, n, &u, "BUSY", &t->panel.busy); pn(b, n, &u, "EN", &t->panel.en);
      u += snprintf(b + u, n - (size_t)u, "spi=5MHz mode0");
      return true;
    case 4:
      u += snprintf(b + u, n - (size_t)u, "FLASH ");
      pn(b, n, &u, "SCK", &t->flash.sck); pn(b, n, &u, "MOSI", &t->flash.mosi); pn(b, n, &u, "MISO", &t->flash.miso); pn(b, n, &u, "CS", &t->flash.cs);
      return true;
    case 5:
      u += snprintf(b + u, n - (size_t)u, "NFC ");
      pn(b, n, &u, "SDA", &t->nfc.sda); pn(b, n, &u, "SCL", &t->nfc.scl); pn(b, n, &u, "PWR", &t->nfc.pwr); pn(b, n, &u, "FD", &t->nfc.fd);
      return true;
    case 6:
      u += snprintf(b + u, n - (size_t)u, "IO ");
      pn(b, n, &u, "B1", &t->btn[0]); pn(b, n, &u, "B2", &t->btn[1]);
      pn(b, n, &u, "R", &t->led[0]); pn(b, n, &u, "G", &t->led[1]); pn(b, n, &u, "B", &t->led[2]);
      return true;
    case 7:
      u += snprintf(b + u, n - (size_t)u, "DBG ");
      pn(b, n, &u, "TX", &t->dbg_tx); pn(b, n, &u, "RX", &t->dbg_rx);
      return true;
    case 8:
      snprintf(b, n, "NFCCHIP present=%u reg25=%02X ln=%u", (unsigned)ni->present, ni->reg25, (unsigned)ni->ndef_len);
      return true;
    case 9:
      u += snprintf(b + u, n - (size_t)u, "NFCAI");
      for (int i = 0; i < 16; i++) u += snprintf(b + u, n - (size_t)u, " %02X", ni->ai[i]);
      return true;
    case 10:
      snprintf(b, n, "NFCTEXT kind=%u cap=%u %.70s", ni->kind, ni->cap, ni->text);
      return true;
    case 11:
      snprintf(b, n, "GPIN A=%04X B=%04X C=%04X D=%04X", (unsigned)GPIO_PortInGet(gpioPortA), (unsigned)GPIO_PortInGet(gpioPortB),
               (unsigned)GPIO_PortInGet(gpioPortC), (unsigned)GPIO_PortInGet(gpioPortD));
      return true;
    default:
      if (part >= 12 && part < 12 + 8) {                   // first 128 bytes of the Solum factory data page
        const uint8_t *p = (const uint8_t *)USERDATA_BASE + (part - 12) * 16;
        u += snprintf(b + u, n - (size_t)u, "UD %02X:", (unsigned)((part - 12) * 16));
        for (int i = 0; i < 16; i++) u += snprintf(b + u, n - (size_t)u, " %02X", p[i]);
        return true;
      }
      if (part >= 20 && part < 20 + 7) {                   // flash: JEDEC id, SFDP header (32 bytes), then 64 bytes of the basic table where its pointer says
        uint8_t id[3], d[16], hd[16];
        if (!xf_begin(id)) { snprintf(b, n, "FLASH id: not answering or busy"); return part == 20; }
        bool more = true;
        if (part == 20) {
          snprintf(b, n, "FLASH JEDEC %02X %02X %02X", id[0], id[1], id[2]);
        } else {
          unsigned o;
          if (part <= 22) {
            o = (unsigned)(part - 21) * 16;
          } else {                                          // header 0 (bytes 8..15) holds the pointer of the basic table in bytes 12..14
            xf_sfdp(0, hd, 16);
            unsigned ptr = (unsigned)hd[12] | ((unsigned)hd[13] << 8) | ((unsigned)hd[14] << 16);
            if (memcmp(hd, "SFDP", 4) != 0 || ptr > 0x3F0) {
              more = false;                                 // no usable table: the card goes on with the trace
            }
            o = ptr + (unsigned)(part - 23) * 16;
          }
          if (more) {
            xf_sfdp(o, d, 16);
            u += snprintf(b + u, n - (size_t)u, "SFDP %02X:", o);
            for (int i = 0; i < 16; i++) u += snprintf(b + u, n - (size_t)u, " %02X", d[i]);
          }
        }
        xf_end();
        return more;
      }
      return false;
  }
}

static bool card_on = false, card_have = false, card_first = true;
static uint8_t card_part = 0, card_pos = 0;
static uint16_t card_off = 0, card_idx = 0;
static rec_t card_rec;

void hwc_card_start(void)
{
  card_on = true; card_part = 0; card_off = 0; card_idx = 0; card_have = false; card_first = true;
  ble_badge_say("CARD fw=%u full=%u", (unsigned)BG_FW_VERSION, (unsigned)tr_full);
}

void hwc_poll(void)
{
  char b[96];
  while (card_on && ble_badge_room()) {
    if (!ble_badge_link_ready()) { card_on = false; break; }
    if (card_part < 40) {
      if (card_line(card_part, b, sizeof b)) { card_part++; ble_badge_say("%s", b); continue; }
      card_part = 40;
    }
    if (!card_have) {
      uint16_t o = card_off;
      if (!next(&o, &card_rec)) { ble_badge_say("CARD end %u records", (unsigned)card_idx); card_on = false; break; }
      card_off = o; card_have = true; card_pos = 0; card_first = true;
    }
    uint8_t per = card_first ? 16 : 24;
    uint8_t k = (uint8_t)(card_rec.nb - card_pos);
    if (k > per) k = per;
    int u;
    if (card_first) u = snprintf(b, sizeof b, "TR %u %lu %c n=%lu b=%u:", (unsigned)card_idx, (unsigned long)card_rec.t, card_rec.type,
                                 (unsigned long)card_rec.total, (unsigned)card_rec.blocks);
    else            u = snprintf(b, sizeof b, "TR %u+:", (unsigned)card_idx);
    for (uint8_t i = 0; i < k; i++) u += snprintf(b + u, sizeof b - (size_t)u, " %02X", card_rec.d[card_pos + i]);
    ble_badge_say("%s", b);
    card_pos = (uint8_t)(card_pos + k);
    card_first = false;
    if (card_pos >= card_rec.nb) { card_have = false; card_idx++; }
  }
}

bool hwc_busy(void) { return card_on; }

#endif
