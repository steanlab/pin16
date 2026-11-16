// pin16: firmware update over Bluetooth, see ota.h. Own code.
#include <string.h>
#include "ota.h"
#include "ext_flash.h"
#include "ble_badge.h"
#include "slot.h"
#include "sysinfo.h"
#include "settings.h"
#include "beacon.h"
#include "led.h"
#include "tapgate.h"
#include "fwver.h"
#include "btl_interface.h"

#define OTA_MIN_MV    2700            // the cell must be at least a quarter full (flashing the application takes a few seconds)
#define GBL_RESIDUE   0x2144DF1CUL    // CRC-32 over a whole GBL file including its last four bytes
#define MARK          "BG22BADG"      // product mark in the unencrypted metadata (build_all.sh, firmware/bg22_fwid.bin)

enum { O_IDLE, O_RUN, O_OK };
static uint8_t  st = O_IDLE;
static uint8_t  win;
static uint16_t seq, inwin;
static uint32_t len, got, crc, ann, erased_to;
static bool     led_on = false;

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

bool ota_is_cmd(uint8_t c) { return c == CMD_OTA; }
bool ota_busy(void) { return st != O_IDLE; }

static void drop(void)
{
  if (st == O_RUN) { xf_ota_mode(false); xf_end(); }
  st = O_IDLE;
  beacon_hold(false);
  if (led_on) { led_stop(); led_on = false; }
}

void ota_link_closed(void) { drop(); }

static bool battery_ok(uint16_t *mv_out)
{
  uint16_t mv = sysinfo_vbat();
  *mv_out = mv;
  return mv == 0 || mv >= OTA_MIN_MV;
}

static void say_status(void)
{
  BootloaderInformation_t bi;
  memset(&bi, 0, sizeof(bi));
  bootloader_getInfo(&bi);
  ble_badge_say("OTA st=%u got=%lu len=%lu btl=%08lX cap=%08lX room=%lu fw=%u minmv=%u", st, (unsigned long)got, (unsigned long)len,
                (unsigned long)bi.version, (unsigned long)bi.capabilities, (unsigned long)XF_OTA_END, (unsigned)BG_FW_VERSION, OTA_MIN_MV);
}

static void fail(const char *why)
{
  drop();
  ble_badge_say("OTA FAIL %s", why);
}

static void begin(const uint8_t *d, uint8_t n)
{
  // 46 01 len32 crc32 win
  if (n < 11) { ble_badge_say("OTA FAIL short"); return; }
  if (slot_busy() || slot_draw_pending()) { ble_badge_say("OTA FAIL busy"); return; }
  if (!tapgate_open()) { ble_badge_say("OTA FAIL tap: touch the tag with the phone, repeat"); return; }
  drop();
  uint32_t l = get32(d + 2);
  if (l < 64 || l > XF_OTA_END) { ble_badge_say("OTA FAIL size %lu (max %lu)", (unsigned long)l, (unsigned long)XF_OTA_END); return; }
  uint16_t mv;
  if (!battery_ok(&mv)) { ble_badge_say("OTA FAIL battery %u mV (need %u)", mv, OTA_MIN_MV); return; }
  uint8_t id[3];
  if (!xf_begin(id)) { ble_badge_say("OTA FAIL flash"); return; }
  xf_ota_mode(true);
  if (!xf_erase_sector(0)) { xf_ota_mode(false); xf_end(); ble_badge_say("OTA FAIL erase"); return; }   // the old image is gone now
  st = O_RUN;
  len = l; ann = get32(d + 6); win = d[10] ? d[10] : 8; got = 0; crc = 0xFFFFFFFFUL; seq = 0; inwin = 0; erased_to = 4096;
  beacon_hold(true);
  if (g_set.led >= 2) { led_pulse(LC_MAGENTA, 1600, 200); led_on = true; }
  uint16_t mtu = ble_badge_mtu();
  uint16_t chunk = mtu > 7 ? (uint16_t)(mtu - 7) : 1;
  if (chunk > 236) chunk = 236;
  ble_badge_say("OTA READY chunk=%u win=%u mtu=%u", chunk, win, mtu);
}

static void data(const uint8_t *d, uint8_t n)
{
  // 46 02 seq16 data...
  if (st != O_RUN) { ble_badge_say("OTA FAIL no upload"); return; }
  if (n < 4) { fail("short"); return; }
  uint16_t s = (uint16_t)(d[2] | (d[3] << 8));
  if (s != seq) { uint16_t want = seq; drop(); ble_badge_say("OTA FAIL seq %u expected %u", s, want); return; }
  uint32_t k = (uint32_t)n - 4;
  if (got + k > len) { fail("too long"); return; }
  while (erased_to < got + k) {                                      // erase the next sector just before it is written (a long erase would stall the link)
    if (!xf_erase_sector(erased_to)) { fail("erase"); return; }
    erased_to += 4096;
  }
  if (!xf_write(got, d + 4, k)) { fail("write"); return; }
  crc = crc_upd(crc, d + 4, k);
  got += k;
  seq++;
  inwin++;
  if (inwin >= win || got == len) {
    inwin = 0;
    ble_badge_say("OTA ACK %lu", (unsigned long)got);
  }
}

// Walks the GBL tags (id32, length32, data) like the bootloader parser; the file ends with the End tag (id 0xFC0404FC, length 4, CRC-32).
// Bytes after the End tag are ignored by the bootloader. Returns the offset after the End tag, 0 = none. Flash open.
static uint32_t gbl_end(uint32_t total)
{
  uint32_t o = 0;
  while (o + 8 <= total) {
    uint8_t h[8];
    xf_read(o, h, 8);
    uint32_t id = get32(h), l = get32(h + 4);
    if (id == 0xFC0404FCUL) return (l == 4 && o + 12 <= total) ? o + 12 : 0;
    if (l > total) return 0;
    o += 8 + l;
  }
  return 0;
}

static void end(void)
{
  if (st != O_RUN) { ble_badge_say("OTA FAIL no upload"); return; }
  if (got != len) { fail("incomplete"); return; }
  uint32_t fin = ~crc;
  if (fin != ann) { fail("crc"); return; }
  uint32_t eo = gbl_end(len);
  if (eo == 0) { fail("no end tag"); return; }
  uint8_t buf[8 + 256];
  uint8_t *b = buf + 8;                                              // the 7 bytes before b[0] carry the end of the previous chunk
  uint32_t rc = 0xFFFFFFFFUL, rg = 0xFFFFFFFFUL, off = 0, carry = 0;
  bool mark = false;
  while (off < len) {                                                // read back what is really in the flash
    uint32_t k = len - off > 256 ? 256 : len - off;
    xf_read(off, b, k);
    if (off == 0 && (k < 16 || b[0] != 0xEB || b[1] != 0x17 || b[2] != 0xA6 || b[3] != 0x03)) { fail("not a gbl file"); return; }   // GBL header tag 0x03A617EB, little-endian
    if (!mark) {                                                     // the product mark may sit anywhere in the file (metadata tag)
      const uint8_t *sp = b - carry;
      uint32_t n = carry + k;
      for (uint32_t i = 0; i + 8 <= n; i++) if (memcmp(sp + i, MARK, 8) == 0) { mark = true; break; }
      carry = n >= 7 ? 7 : n;
      memmove(b - carry, sp + n - carry, carry);
    }
    rc = crc_upd(rc, b, k);
    if (off < eo) rg = crc_upd(rg, b, off + k <= eo ? k : eo - off);   // only up to the End tag
    off += k;
  }
  if (!mark) { fail("wrong product"); return; }
  if (~rc != ann) { fail("readback"); return; }
  // the GBL file carries its own CRC-32 in the last tag: over the whole file the residue is constant
  if (rg != (GBL_RESIDUE ^ 0xFFFFFFFFUL)) { fail("gbl crc"); return; }
  xf_ota_mode(false);
  xf_end();
  st = O_OK;
  if (led_on) { led_stop(); led_on = false; }
  ble_badge_say("OTA OK len=%lu crc=%08lX gbl=1 product=1", (unsigned long)len, (unsigned long)ann);
}

static void install(const uint8_t *d, uint8_t n)
{
  if (n < 3 || d[2] != 0xA5) { ble_badge_say("OTA FAIL arg"); return; }
  if (st != O_OK) { ble_badge_say("OTA FAIL not verified"); return; }
  uint16_t mv;
  if (!battery_ok(&mv)) { ble_badge_say("OTA FAIL battery %u mV (need %u)", mv, OTA_MIN_MV); return; }
  ble_badge_say("OTA INSTALL bootloader takes over, reconnect in about 40 s");
  ble_badge_reboot_install();
}

void ota_cmd(const uint8_t *d, uint8_t len_)
{
  if (len_ < 2) { ble_badge_say("OTA FAIL short"); return; }
  switch (d[1]) {
    case 0x00: say_status(); break;
    case 0x01: begin(d, len_); break;
    case 0x02: data(d, len_); break;
    case 0x03: end(); break;
    case 0x04: install(d, len_); break;
    case 0x05: if (st != O_IDLE) { drop(); ble_badge_say("OTA ABORT"); } else ble_badge_say("OTA idle"); break;
    default: ble_badge_say("ERR cmd 46 %02X", d[1]); break;
  }
}
