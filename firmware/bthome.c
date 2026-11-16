// pin16: optional BTHome v2 beacon, see bthome.h. Own code.
#include <string.h>
#include "sl_bluetooth.h"
#include "sl_bt_api.h"
#include "sl_sleeptimer.h"
#include "bthome.h"
#include "settings.h"
#include "sysinfo.h"
#include "slot.h"
#include "ble_badge.h"
#include "hw.h"

#define BURST_EVENTS   3             // advertising events per beacon
#define BURST_ITV      160           // 100 ms in units of 0.625 ms

static uint8_t bt_set = 0xFF;
static bool ready = false;
static volatile bool due = false;
static bool sending = false;
static uint8_t pkt_id = 0;
static sl_sleeptimer_timer_handle_t timer;
static bool timer_on = false;
static const uint16_t itv_s[4] = { 10, 30, 60, 300 };
static char nm[30];
static bool win_mod = false;                         // the connectable set currently carries BTHome data

static void on_timer(sl_sleeptimer_timer_handle_t *h, void *d)
{
  (void)h; (void)d;
  due = true;
}

void bthome_init(const char *name)
{
  sl_status_t sc = sl_bt_advertiser_create_set(&bt_set);
  if (sc != SL_STATUS_OK) {
    ble_badge_ev(EV_ERR, 1, (uint16_t)sc);
    return;
  }
  uint8_t sr[31];
  size_t n = strlen(name);
  if (n > 29) n = 29;
  memcpy(nm, name, n); nm[n] = 0;
  sr[0] = (uint8_t)(n + 1);
  sr[1] = 0x09;                                      // complete local name
  memcpy(sr + 2, name, n);
  sc = sl_bt_legacy_advertiser_set_data(bt_set, sl_bt_advertiser_scan_response_packet, n + 2, sr);
  if (sc != SL_STATUS_OK) ble_badge_ev(EV_ERR, 2, (uint16_t)sc);
  ready = true;
  bthome_apply();
}

void bthome_apply(void)
{
  if (timer_on) {
    sl_sleeptimer_stop_timer(&timer);
    timer_on = false;
  }
  if (!ready) return;
  if (g_set.bt_en) {
    sl_sleeptimer_start_periodic_timer_ms(&timer, (uint32_t)itv_s[g_set.bt_itv & 3] * 1000UL, on_timer, NULL, 0, 0);
    timer_on = true;
    due = true;                                      // first beacon right away
  } else {
    due = false;
    bthome_quiet();
  }
}

void bthome_on_timeout(uint8_t handle)
{
  if (handle == bt_set) sending = false;
}

void bthome_quiet(void)
{
  if (ready && sending) {
    sl_bt_advertiser_stop(bt_set);
    sending = false;
  }
}

bool bthome_pending(void) { return due; }

static uint8_t put_u8(uint8_t *p, uint8_t id, uint8_t v) { p[0] = id; p[1] = v; return 2; }
static uint8_t put_u16(uint8_t *p, uint8_t id, uint16_t v) { p[0] = id; p[1] = (uint8_t)v; p[2] = (uint8_t)(v >> 8); return 3; }

static uint8_t build(uint8_t *d)                // BTHome service data element, returns its length
{
  uint16_t mv = sysinfo_vbat();
  uint16_t mn = sysinfo_min_load();
  int8_t tc = hw_temperature();
  uint8_t m = g_set.bt_mask;

  uint8_t i = 0;
  d[i++] = 0;                                        // length, filled in below
  d[i++] = 0x16;                                     // service data, 16-bit UUID
  d[i++] = 0xD2;                                     // 0xFCD2 = BTHome
  d[i++] = 0xFC;
  d[i++] = 0x40;                                     // BTHome v2, not encrypted, regular interval
  // objects in ascending order of their id
  i += put_u8(d + i, 0x00, pkt_id++);                // packet id
  if (m & 0x01) i += put_u8(d + i, 0x01, sysinfo_percent(mv));
  if (m & 0x02) i += put_u16(d + i, 0x02, (uint16_t)(int16_t)((int)tc * 100));          // chip temperature, 0.01 degC
  if (m & 0x10) { int s = slot_current(); i += put_u8(d + i, 0x09, s < 0 ? 0xFF : (uint8_t)s); }   // slide on the panel
  if (m & 0x04) i += put_u16(d + i, 0x0C, mv);                                          // supply, mV
  if ((m & 0x08) && mn) i += put_u16(d + i, 0x0C, mn);                                  // lowest supply during the last draw
  if (m & 0x40) i += put_u8(d + i, 0x15, sysinfo_low(mv) ? 1 : 0);                      // battery low
  if (m & 0x20) i += put_u16(d + i, 0x3D, (uint16_t)sysinfo_draws());                   // panel draws since power-up
  d[0] = (uint8_t)(i - 1);
  return i;
}

void bthome_process(void)
{
  if (!due) return;
  due = false;
  if (!ready || !g_set.bt_en || sending) return;
  if (ble_badge_is_active() || slot_draw_pending()) return;          // this round is skipped, the next one comes

  uint8_t d[31];
  uint8_t i = build(d);

  sl_status_t sc = sl_bt_legacy_advertiser_set_data(bt_set, sl_bt_advertiser_advertising_data_packet, i, d);
  if (sc == SL_STATUS_OK) sc = sl_bt_advertiser_set_timing(bt_set, BURST_ITV, BURST_ITV, 0, BURST_EVENTS);
  if (sc == SL_STATUS_OK) sc = sl_bt_legacy_advertiser_start(bt_set, sl_bt_legacy_advertiser_scannable);
  if (sc == SL_STATUS_OK) {
    sending = true;
    ble_badge_ev(EV_BEACON, pkt_id, i);
  } else {
    ble_badge_ev(EV_ERR, 3, (uint16_t)sc);
  }
}

void bthome_kick(void)
{
  if (ready && g_set.bt_en) due = true;
}

void bthome_window_data(uint8_t adv_set)
{
  if (!ready) return;
  if (g_set.bt_en && g_set.bt_win) {
    uint8_t d[31];
    uint8_t i = 0;
    d[i++] = 2; d[i++] = 0x01; d[i++] = 0x06;          // flags: general discoverable, no BR/EDR
    i += build(d + i);
    uint8_t sr[31];
    size_t n = strlen(nm);
    sr[0] = (uint8_t)(n + 1); sr[1] = 0x09; memcpy(sr + 2, nm, n);
    sl_status_t sc = sl_bt_legacy_advertiser_set_data(adv_set, sl_bt_advertiser_advertising_data_packet, i, d);
    if (sc == SL_STATUS_OK) sc = sl_bt_legacy_advertiser_set_data(adv_set, sl_bt_advertiser_scan_response_packet, n + 2, sr);
    if (sc != SL_STATUS_OK) ble_badge_ev(EV_ERR, 4, (uint16_t)sc);
    win_mod = true;
  } else if (win_mod) {
    sl_bt_legacy_advertiser_generate_data(adv_set, sl_bt_advertiser_general_discoverable);
    win_mod = false;
  }
}
