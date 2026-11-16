// pin16: OpenDisplay subset, see od.h. Frame of the panel (200 x 200 or 160 x 296), four colours, 2 bits per pixel, black 0 white 1 yellow 2 red 3.
// Direct write (70, 71..., 72) stores an uncompressed frame in the slot g_set.od_slot and draws it; 00 73 follows when the refresh is over.
#include <string.h>
#include "sl_bt_api.h"
#include "od.h"
#include "settings.h"
#include "slot.h"
#include "ble_badge.h"
#include "sysinfo.h"
#include "panel.h"
#include "hw.h"
#include "fwver.h"

#define OD_FW_MAJOR 1
#define OD_FW_MINOR 0
static const char od_fw_tag[] = "pin16";
static bool od_on = false;

bool od_muted(void) { return od_on; }
void od_link_down(void) { od_on = false; }

static void reply(uint8_t status, uint8_t cmd, const uint8_t *data, uint8_t n)
{
  uint8_t b[32];
  if (n > 30) n = 30;
  b[0] = status; b[1] = cmd;
  if (n) memcpy(b + 2, data, n);
  ble_badge_send(b, (uint8_t)(2 + n));
}
static void ok(uint8_t cmd) { reply(0x00, cmd, NULL, 0); }
static void nack(uint8_t cmd) { reply(0xFF, cmd, NULL, 0); }

static uint16_t crc16(const uint8_t *p, int n)
{
  uint16_t c = 0xFFFF;
  while (n--) {
    c ^= (uint16_t)(*p++) << 8;
    for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
  }
  return c;
}

static uint8_t cfg[160];
static int cfg_build(void)
{
  int n = 0;
  memset(cfg, 0, sizeof(cfg));
  cfg[n++] = 0; cfg[n++] = 0;                    // length, patched below
  cfg[n++] = 1;                                  // protocol major version
  // packet 1: system_config (22 bytes)
  cfg[n++] = 0; cfg[n++] = 1;
  cfg[n++] = 5; cfg[n++] = 0;                    // ic_type (closest entry of the list)
  cfg[n++] = 0x01;                               // communication_modes: BLE
  cfg[n++] = 0x00;                               // device_flags
  cfg[n++] = 0xFF;                               // pwr_pin
  n += 15;                                       // reserved
  cfg[n++] = 0xFF; cfg[n++] = 0xFF;              // pwr_pin_2, pwr_pin_3
  // packet 2: manufacturer_data (22 bytes): diy, custom board
  cfg[n++] = 1; cfg[n++] = 2;
  n += 22;
  // packet 4: power_option (30 bytes): battery, lithium primary, no sense pins
  cfg[n++] = 2; cfg[n++] = 4;
  cfg[n++] = 1;
  n += 3 + 2 + 1 + 1;
  cfg[n++] = 0xFF; cfg[n++] = 0xFF;
  n += 1;
  cfg[n++] = 4;
  n += 2 + 4 + 2;
  cfg[n++] = 0xFF; cfg[n++] = 0xFF;
  n += 1 + 2 + 1 + 4;
  // packet 32: display (46 bytes): the scan size of the panel, four colours, direct write
  cfg[n++] = 3; cfg[n++] = 32;
  cfg[n++] = 0;                                  // instance
  cfg[n++] = 1;                                  // e-paper
  n += 2;                                        // panel_ic_type unknown
  cfg[n++] = (uint8_t)panel_w(); cfg[n++] = (uint8_t)(panel_w() >> 8);       // pixel_width
  cfg[n++] = (uint8_t)panel_h(); cfg[n++] = (uint8_t)(panel_h() >> 8);       // pixel_height
  n += 2 + 2 + 2;                                // active_width_mm, active_height_mm, legacy_tagtype
  cfg[n++] = 0;                                  // rotation
  cfg[n++] = 0xFF; cfg[n++] = 0xFF; cfg[n++] = 0xFF; cfg[n++] = 0xFF; cfg[n++] = 0xFF;
  cfg[n++] = 0;                                  // partial_update_support
  cfg[n++] = 3;                                  // color_scheme: bwry
  cfg[n++] = 0x08;                               // transmission_modes: direct_write only
  cfg[n++] = 0xFF; cfg[n++] = 0xFF;
  cfg[n++] = 0xFF; cfg[n++] = 0xFF; cfg[n++] = 0xFF; cfg[n++] = 0xFF; cfg[n++] = 0xFF; cfg[n++] = 0xFF;
  n += 2 + 13;
  int total = n + 2;
  cfg[0] = (uint8_t)total; cfg[1] = (uint8_t)(total >> 8);
  cfg[0] = cfg[1] = 0;                           // the CRC is taken with the length bytes zeroed
  uint16_t c = crc16(cfg, n);
  cfg[0] = (uint8_t)total; cfg[1] = (uint8_t)(total >> 8);
  cfg[n++] = (uint8_t)c; cfg[n++] = (uint8_t)(c >> 8);
  return n;
}

static void config_read(void)
{
  int n = cfg_build();
  int ch = (int)ble_badge_mtu() - 3 - 4 - 2;
  if (ch > 48) ch = 48;
  if (ch < 8) ch = 8;
  int off = 0, idx = 0;
  while (off < n) {
    uint8_t b[64];
    int k = 0;
    b[k++] = 0x00; b[k++] = 0x40; b[k++] = (uint8_t)idx; b[k++] = (uint8_t)(idx >> 8);
    if (idx == 0) { b[k++] = (uint8_t)n; b[k++] = (uint8_t)(n >> 8); }
    int m = n - off < ch ? n - off : ch;
    memcpy(b + k, cfg + off, (size_t)m);
    k += m;
    ble_badge_send(b, (uint8_t)k);
    off += m; idx++;
  }
}

// 16 bytes, layout of the reference firmware (see od.h). The loop counter changes with every advertisement update.
static uint8_t msd_loop = 0;
void od_msd(uint8_t b[16])
{
  od_msd_pack(b, sysinfo_vbat(), hw_temperature(), msd_loop);
}

void od_adv_data(uint8_t adv_set)
{
  uint8_t d[31], sr[31], m[16];
  uint8_t i = 0;
  msd_loop = (uint8_t)((msd_loop + 1) & 15);
  od_msd(m);
  d[i++] = 2; d[i++] = 0x01; d[i++] = 0x06;                      // flags
  d[i++] = 3; d[i++] = 0x03; d[i++] = 0x46; d[i++] = 0x24;       // complete list of 16-bit service UUIDs: 0x2446
  d[i++] = 17; d[i++] = 0xFF; memcpy(d + i, m, 16); i += 16;     // manufacturer specific data, company id 0x2446 first
  uint8_t n = 0;
  const char *nm = ble_badge_od_name();
  sr[n++] = (uint8_t)(strlen(nm) + 1); sr[n++] = 0x09;
  memcpy(sr + n, nm, strlen(nm)); n = (uint8_t)(n + strlen(nm));
  sl_bt_legacy_advertiser_set_data(adv_set, sl_bt_advertiser_advertising_data_packet, i, d);
  sl_bt_legacy_advertiser_set_data(adv_set, sl_bt_advertiser_scan_response_packet, n, sr);
}

void od_rx(const uint8_t *d, uint8_t len)
{
  if (len < 2) return;
  od_on = true;
  switch (d[1]) {
    case 0x40: config_read(); break;
    case 0x43: {
      uint8_t b[24];                                       // [major][minor][shaLen][sha...][patch] (reference firmware)
      int L = (int)sizeof(od_fw_tag) - 1, k = 0;
      b[k++] = OD_FW_MAJOR; b[k++] = OD_FW_MINOR; b[k++] = (uint8_t)L;
      memcpy(b + k, od_fw_tag, (size_t)L); k += L;
      b[k++] = BG_FW_VERSION;
      reply(0x00, 0x43, b, (uint8_t)k);
      break;
    }
    case 0x44: {
      uint8_t b[16];
      od_msd(b);
      reply(0x00, 0x44, b, 16);
      break;
    }
    case 0x50: { uint8_t s = 0x03; reply(0x00, 0x50, &s, 1); break; }   // authenticate: no encryption configured
    case 0x70:
      if (len > 2) { nack(0x70); break; }        // a size field means compressed data: not offered
      if (slot_od_begin()) ok(0x70); else nack(0x70);
      break;
    case 0x71:
      if (slot_od_data(d + 2, (uint32_t)len - 2)) ok(0x71); else nack(0x71);
      break;
    case 0x72:
      if (slot_od_end()) { ok(0x72); slot_request_draw(g_set.od_slot, true); } else nack(0x72);
      break;
    case 0x0F: ble_badge_reboot(); break;
    default: nack(d[1]); break;
  }
}
