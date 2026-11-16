// pin16: Bluetooth LE module. Stage 1: advertising window + GATT echo/info test.
// Radio is used only while the window is open (120 s after power-up and after NFC tap).
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "sl_bluetooth.h"
#include "sl_bt_api.h"
#include "gatt_db.h"
#if __has_include("bg22_gattdb_inline.h")
#include "gatt_db.c"   // gatt_db.c made by hand in build_firmware.sh, not by slc
#endif
#include "sl_sleeptimer.h"
#include "sl_udelay.h"
#include "sl_power_manager.h"
#include "ble_badge.h"
#include "ext_flash.h"
#include "slot.h"
#include "settings.h"
#include "od.h"
#include "cmd_set.h"
#include "em_device.h"
#include "hw.h"
#include "sysinfo.h"
#include "bthome.h"
#include "pics.h"
#include "fwver.h"
#include "em_core.h"
#include "led.h"
#include "ota.h"
#include "beacon.h"
#include "hwcard.h"
#include "btl_interface.h"

#define FW_VERSION      BG_FW_VERSION

#define ADV_INTERVAL    48             // units of 0.625 ms = 30 ms (a phone finds it and connects faster)
#define NO_CONN         0xFF

static uint8_t adv_set = 0xFF;
static uint8_t conn = NO_CONN;
static bool stack_ready = false;
static bool advertising = false;
static bool window_open = false;
static bool notify_on = false;
static char od_nm[12] = "OD";
const char *ble_badge_od_name(void) { return od_nm; }
static bool od_via = false;            // the client talks OpenDisplay on the standard service 0x2446: answers go out on that characteristic
static bool od_notify = false;         // that client enabled notifications
#define OUT_NOTIFY() (od_via ? od_notify : notify_on)
#define OUT_CHAR()   (od_via ? gattdb_od_char : gattdb_tx)
static volatile uint8_t open_req = 0;      // 1 = window of a tap, 2 = window after power-up
static volatile bool tick_flag = false;
static uint32_t window_end = 0;        // ms
static uint32_t last_activity = 0;
static sl_sleeptimer_timer_handle_t tick_timer;
static bool em1_held = false;
static uint16_t last_close_reason = 0;
static uint16_t att_mtu = 23;
static uint32_t conn_open_ms = 0;      // when the current link opened
static uint32_t close_req_ms = 0;      // when the tag asked to close this link (do not ask again for 3 s)
static bool     got_rx = false;        // the client has sent a command on this link

// ---- text lines: small queue, drained from the main loop --------------------
#define QN   8
#define QLEN 96
static char     q_buf[QN][QLEN];
static uint8_t  q_len[QN];
static uint8_t  q_head = 0, q_cnt = 0;

uint16_t ble_badge_mtu(void) { return att_mtu; }

static void q_drain(void)
{
  while (q_cnt && conn != NO_CONN && OUT_NOTIFY()) {
    uint8_t n = q_len[q_head];
    uint16_t maxn = att_mtu > 3 ? (uint16_t)(att_mtu - 3) : 20;
    if (n > maxn) n = (uint8_t)maxn;
    sl_status_t sc = sl_bt_gatt_server_send_notification(conn, OUT_CHAR(), n, (uint8_t *)q_buf[q_head]);
    if (sc != SL_STATUS_OK) {
      break;                                  // stack buffers full: try again from the main loop
    }
    q_head = (uint8_t)((q_head + 1) % QN);
    q_cnt--;
  }
  if (conn == NO_CONN || !OUT_NOTIFY()) {
    q_cnt = 0;                                // nobody to read them
  }
}

static void q_put(const uint8_t *p, uint8_t n)
{
  for (int tries = 0; q_cnt >= QN && tries < 600; tries++) {
    q_drain();
    if (q_cnt >= QN) sl_udelay_wait(1000);
  }
  if (q_cnt >= QN) return;
  uint8_t idx = (uint8_t)((q_head + q_cnt) % QN);
  if (n > QLEN) n = QLEN;
  memcpy(q_buf[idx], p, n);
  q_len[idx] = n;
  q_cnt++;
  q_drain();
}

bool ble_badge_room(void) { return q_cnt < QN - 1; }
bool ble_badge_link_ready(void) { return conn != NO_CONN && OUT_NOTIFY(); }

void ble_badge_send(const uint8_t *p, uint8_t n)
{
  if (conn == NO_CONN) return;
  q_put(p, n);
}

void ble_badge_say(const char *fmt, ...)
{
  if (conn == NO_CONN || od_muted()) return;
  for (int tries = 0; q_cnt >= QN && tries < 600; tries++) {   // wait up to ~600 ms for the link to take lines
    q_drain();
    if (q_cnt >= QN) sl_udelay_wait(1000);
  }
  if (q_cnt >= QN) return;                    // dropped
  uint8_t idx = (uint8_t)((q_head + q_cnt) % QN);
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(q_buf[idx], QLEN, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n >= QLEN) n = QLEN - 1;
  q_len[idx] = (uint8_t)n;
  q_cnt++;
  q_drain();
}

// While the window is open the CPU stays in EM1, so that the radio timing runs from the
// 38.4 MHz crystal and not from the inaccurate LFRCO sleep clock (the tags have no 32 kHz crystal).
static void hold_em1(bool hold)
{
  if (hold && !em1_held) {
    sl_power_manager_add_em_requirement(SL_POWER_MANAGER_EM1);
    em1_held = true;
  } else if (!hold && em1_held) {
    sl_power_manager_remove_em_requirement(SL_POWER_MANAGER_EM1);
    em1_held = false;
  }
}

static void on_tick(sl_sleeptimer_timer_handle_t *h, void *d)
{
  (void)h; (void)d;
  tick_flag = true;
}

static uint32_t now_ms(void)
{
  uint64_t ms = 0;                                    // 64-bit tick counter: no wrap after 36 hours
  sl_sleeptimer_tick64_to_ms(sl_sleeptimer_get_tick_count64(), &ms);
  return (uint32_t)ms;
}

// Small event log (ring of 64), read with command 4F 09: helps to see what happens on a link that hangs.
typedef struct { uint32_t t; uint8_t id; uint16_t a, b; } ev_t;
#define EVN 64
static ev_t evl[EVN];
static uint8_t ev_head = 0, ev_cnt = 0;

static void ev_add(uint8_t id, uint16_t a, uint16_t b)
{
  ev_t *e = &evl[(ev_head + ev_cnt) % EVN];
  if (ev_cnt == EVN) {
    ev_head = (uint8_t)((ev_head + 1) % EVN);
  } else {
    ev_cnt++;
  }
  e->t = now_ms(); e->id = id; e->a = a; e->b = b;
}

void ble_badge_ev(uint8_t id, uint16_t a, uint16_t b) { ev_add(id, a, b); }

static void close_window(void);
static bool close_win_req = false;                    // after the link: no window, straight to the beacon

void ble_badge_close_link(void)
{
  if (conn != NO_CONN) {
    sl_bt_connection_close(conn);
  }
}

void ble_badge_close_link_and_window(void)
{
  close_win_req = true;
  if (conn != NO_CONN) sl_bt_connection_close(conn);
  else { close_win_req = false; if (window_open) close_window(); }
}

void ble_badge_touch(void) { last_activity = now_ms(); }

// The event list is sent line by line from the main loop (about 65 lines), never in one go inside the Bluetooth callback.
static uint8_t dump_k = 0, dump_n = 0, dump_base = 0;
static bool dump_on = false;

void ble_badge_dump_events(void)
{
  dump_n = ev_cnt; dump_base = ev_head; dump_k = 0; dump_on = true;
  ble_badge_say("EVENTS %u", ev_cnt);
}

static void dump_step(void)
{
  static const char *const nm[] = { "?", "boot", "open", "close", "mtu", "char", "rx", "param", "adv", "advtmo", "win", "fixlat", "rssi", "idle", "beacon", "err" };
  while (dump_on && q_cnt < QN - 1) {
    if (conn == NO_CONN || !notify_on || dump_k >= dump_n) { dump_on = false; break; }
    ev_t *e = &evl[(dump_base + dump_k) % EVN];
    dump_k++;
    ble_badge_say("EV %lu.%03lu %s %u %u", (unsigned long)(e->t / 1000), (unsigned long)(e->t % 1000), nm[e->id < 16 ? e->id : 0], e->a, e->b);
  }
}

// ---- LED signals (engine in led.c; level setting: 0 off, 1 errors and battery, 2 normal states, 3 plus diagnostics) ----
//   blue flash: tap / window open, every 5 s while the window waits; 2 blue: window over
//   green flash: link made, every 5 s while linked and quiet; green pulse: commands and data are flowing
//   amber pulse: the panel is being drawn; 2 green: drawn; 3 red: failed; 3 red at window open: battery low
//   white at power-up, 2 red after a crash restart, 3 cyan at power-up in OpenDisplay mode; red flashes after a link: close reason (level 3)
static uint8_t led_kind = 0;               // 0 nothing running, 1 green pulse (exchange), 2 amber pulse (panel work)
static uint32_t last_xchg = 0;             // last command that was not a keep-alive ping
static uint8_t hb = 0;                     // seconds towards the next 5 s heartbeat

static void led_event(uint8_t col, uint16_t ms, uint8_t level)
{
  if (g_set.led >= level) led_flash(col, 1, ms, 0);
}

void ble_badge_led_work(uint8_t what)      // 0 panel work starts, 1 finished well, 2 failed, 3 finished, no signal
{
  if (what == 0) {
    if (g_set.led >= 2) { led_pulse(LC_AMBER, 2000, 160); led_kind = 2; }
  } else {
    if (led_kind == 2) { led_stop(); led_kind = 0; }
    if (what == 3) { /* the boot splash: the window signal follows */ }
    else if (what == 1) { if (g_set.led >= 2) led_flash(LC_GREEN, 2, 80, 120); }
    else if (g_set.led >= 1) led_flash(LC_RED, 3, 300, 200);
  }
}

static void start_advertising(void)
{
  if (!stack_ready || advertising || conn != NO_CONN) {
    return;
  }
  uint32_t left = (uint32_t)g_set.wb * 1000UL;
  uint32_t t = now_ms();
  if ((int32_t)(window_end - t) > 0) {
    left = window_end - t;
  }
  if (g_set.opmode == 1) {
    od_adv_data(adv_set);                             // OpenDisplay mode: service 0x2446 and the 16-byte record of the protocol
  } else {
    bthome_window_data(adv_set);                      // optional: BTHome data in the advertisement of the window
  }
  uint16_t duration = (uint16_t)(left / 10UL);        // units of 10 ms
  if (duration == 0) {
    duration = 1;
  }
  sl_status_t sc = sl_bt_advertiser_set_timing(adv_set, ADV_INTERVAL, ADV_INTERVAL, duration, 0);
  if (sc == SL_STATUS_OK) sc = sl_bt_legacy_advertiser_start(adv_set, sl_bt_legacy_advertiser_connectable);
  advertising = (sc == SL_STATUS_OK);
  ev_add(EV_ADV, (uint16_t)sc, duration);
}

static void stop_advertising(void)
{
  if (advertising) {
    sl_bt_advertiser_stop(adv_set);
    advertising = false;
  }
}

static void open_window_ms(uint32_t ms)
{
  window_end = now_ms() + ms;
  bthome_quiet();                                    // no beacon while a window is open
  hold_em1(true);
  if (!window_open) {
    window_open = true;
    sl_sleeptimer_start_periodic_timer_ms(&tick_timer, 1000, on_tick, NULL, 0, 0);
  }
  start_advertising();
}

static void open_window(void)
{
  open_window_ms((uint32_t)g_set.wb * 1000UL);
}

static void close_window(void)
{
  stop_advertising();
  window_open = false;
  hold_em1(false);
  sl_sleeptimer_stop_timer(&tick_timer);
  bthome_kick();                                      // the first beacon goes out as soon as the window is closed
}

static void send_response(uint8_t cmd, uint8_t status, const uint8_t *payload, uint8_t len)
{
  uint8_t buf[64];
  if (len > sizeof(buf) - 2) {
    len = sizeof(buf) - 2;
  }
  buf[0] = cmd | 0x80;
  buf[1] = status;
  if (len) {
    memcpy(buf + 2, payload, len);
  }
  sl_bt_gatt_server_write_attribute_value(gattdb_tx, 0, (uint32_t)len + 2, buf);
  if (notify_on && conn != NO_CONN) {
    sl_bt_gatt_server_send_notification(conn, gattdb_tx, (uint32_t)len + 2, buf);
  }
}

static void handle_command(const uint8_t *data, uint8_t len)
{
  if (len == 0) {
    return;
  }
  uint8_t cmd = data[0];
  if (cmd == 0x00) {
    od_rx(data, len);
    return;
  }
  if (cmdset_is(cmd)) {
    cmdset_run(data, len);
    return;
  }
  if (ota_is_cmd(cmd)) {
    ota_cmd(data, len);
    return;
  }
  if (slot_is_cmd(cmd)) {
    slot_cmd(data, len);
    return;
  }
  switch (cmd) {
    case BCMD_PING: {
      static const uint8_t pong[] = { 'P', 'O', 'N', 'G' };
      send_response(cmd, BSTAT_OK, pong, sizeof(pong));
      break;
    }
    case BCMD_INFO: {
      uint16_t mv = sysinfo_vbat();
      uint8_t p[7];
      p[0] = FW_VERSION & 0xFF;
      p[1] = FW_VERSION >> 8;
      p[2] = mv & 0xFF;
      p[3] = mv >> 8;
      p[4] = hw_tag_type();
      p[5] = last_close_reason & 0xFF;
      p[6] = last_close_reason >> 8;
      send_response(cmd, BSTAT_OK, p, sizeof(p));
      break;
    }
    case BCMD_ECHO:
      send_response(cmd, BSTAT_OK, data + 1, len - 1);
      break;
    case BCMD_LED:
      if (len != 2) {
        send_response(cmd, BSTAT_BADLEN, NULL, 0);
      } else {
        led_rgb((data[1] & 0x03) ? 255 : 0, (data[1] & 0xE0) ? 255 : 0, (data[1] & 0x1C) ? 255 : 0, 200);
        send_response(cmd, BSTAT_OK, NULL, 0);
      }
      break;
    case BCMD_FJEDEC: {
      uint8_t id[3] = { 0, 0, 0 };
      bool ok = xf_begin(id);
      if (ok) {
        xf_end();
      }
      send_response(cmd, ok ? BSTAT_OK : 0x01, id, 3);
      break;
    }
    case BCMD_FREAD: {
      if (len != 5 || data[4] == 0 || data[4] > 50) {
        send_response(cmd, BSTAT_BADLEN, NULL, 0);
        break;
      }
      uint8_t id[3];
      uint8_t buf[50];
      if (!xf_begin(id)) {
        send_response(cmd, 0x01, NULL, 0);
        break;
      }
      xf_read(((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3], buf, data[4]);
      xf_end();
      send_response(cmd, BSTAT_OK, buf, data[4]);
      break;
    }
    case BCMD_FTEST: {
      uint16_t er, pr, bad;
      uint8_t r = xf_selftest(&er, &pr, &bad);
      uint8_t p[6] = { (uint8_t)er, (uint8_t)(er >> 8), (uint8_t)pr, (uint8_t)(pr >> 8), (uint8_t)bad, (uint8_t)(bad >> 8) };
      send_response(cmd, r, p, sizeof(p));
      break;
    }
    default:
      send_response(cmd, BSTAT_BADCMD, NULL, 0);
      break;
  }
}

static const int16_t tx_table[6] = { -150, -100, -50, 0, 30, 60 };   // 0.1 dBm
static volatile uint32_t reboot_at = 0;
static volatile bool reboot_btl = false;       // restart into the bootloader, which installs the staged firmware image

void ble_badge_apply_tx(void)
{
  int16_t a, b;
  sl_bt_system_set_tx_power(tx_table[g_set.txp < 6 ? g_set.txp : 3], tx_table[g_set.txp < 6 ? g_set.txp : 3], &a, &b);
}

void ble_badge_reboot_install(void)
{
  reboot_btl = true;
  ble_badge_reboot();
}

void ble_badge_reboot(void)
{
  reboot_at = now_ms() + 500;
  if (reboot_at == 0) reboot_at = 1;
}

// ----------------------------------------------------------------------------
void ble_badge_request_window(void)
{
  if (open_req == 0) open_req = 1;
}

void ble_badge_request_boot_window(void)
{
  open_req = 2;
}

bool ble_badge_event_pending(void)
{
  return open_req || tick_flag || q_cnt != 0 || slot_draw_pending() || reboot_at != 0 || dump_on || sysinfo_pending() || bthome_pending() || beacon_pending();
}

bool ble_badge_is_active(void)
{
  return window_open || conn != NO_CONN;
}

void ble_badge_process(void)
{
  sysinfo_poll();
  beacon_poll();
  if (q_cnt) {
    q_drain();
  }
  if (dump_on) {
    dump_step();
  }
#ifdef BG_DIAG
  hwc_poll();
#endif
  if (reboot_at != 0 && q_cnt == 0 && (int32_t)(now_ms() - reboot_at) >= 0) {
    if (reboot_btl) bootloader_rebootAndInstall();
    NVIC_SystemReset();
  }
  if (open_req) {
    uint8_t k = open_req;
    open_req = 0;
    if (stack_ready) {
      open_window_ms((uint32_t)(k == 2 ? g_set.wb : g_set.wt) * 1000UL);
      if (g_set.led >= 1 && sysinfo_low(sysinfo_vbat())) {
        led_flash(LC_RED, 3, 150, 150);                    // battery nearly empty
      } else {
        led_event(LC_BLUE, 100, 2);
      }
    }
  }
  if (tick_flag) {
    tick_flag = false;
    uint32_t t = now_ms();
    if (led_kind != 0 && !led_pulsing()) led_kind = 0;                             // a flash replaced our pulse: do not stay blocked
    if (led_kind == 1 && t - last_xchg > 1500) { led_stop(); led_kind = 0; }       // the exchange is over: pulse ends
    if (led_kind == 0 && !led_pulsing()) {
      if (beacon_active()) hb = 0;                                                                   // the beacon pattern owns the LED: no heartbeat flashes in between
      else if (++hb >= 5) {                                                                          // heartbeat every 5 s: blue = window waits, green = linked
        hb = 0;
        if (conn == NO_CONN) { if (window_open) led_event(LC_BLUE, 40, 2); } else led_event(LC_GREEN, 40, 2);
      }
    }
    if (conn != NO_CONN) {
      bool again = close_req_ms == 0 || t - close_req_ms > 3000UL;
      if (!got_rx && t - conn_open_ms > BLE_FIRST_MS) {
        // a phone that opened a link but never sent a command: drop it, so the next attempt starts on a fresh link
        if (again) {
          ev_add(EV_IDLE, 1, (uint16_t)((t - conn_open_ms) / 1000));
          close_req_ms = t ? t : 1;
          sl_bt_connection_close(conn);
        }
      } else if (t - last_activity > BLE_IDLE_MS) {
        if (again) {
          ev_add(EV_IDLE, 2, (uint16_t)((t - last_activity) / 1000));
          close_req_ms = t ? t : 1;
          sl_bt_connection_close(conn);
        }
      }
    } else if (window_open && (int32_t)(t - window_end) >= 0) {
      if (g_set.opmode == 1) {
        window_end = t + 60000UL;          // OpenDisplay mode: always connectable
        start_advertising();
      } else {
        if (g_set.led >= 2) led_flash(LC_BLUE, 2, 40, 120);       // the window is over
        close_window();
      }
    } else if (window_open && !advertising) {
      start_advertising();                 // the advertiser ran out before the window did
    }
  }
  bthome_process();
  slot_poll();                             // last: a draw blocks for ~20 s, the blue flash and the window come first
}

// Bluetooth stack events (called from sl_system_process_action)
void sl_bt_on_event(sl_bt_msg_t *evt)
{
  switch (SL_BT_MSG_ID(evt->header)) {
    case sl_bt_evt_system_boot_id: {
      bd_addr addr;
      uint8_t type;
      char name[24];
      int16_t txmin, txmax;
      (void)txmin; (void)txmax;
      ble_badge_apply_tx();
      sl_bt_system_get_identity_address(&addr, &type);
      if (g_set.name[0]) {
        snprintf(name, sizeof(name), "pin16-%s", g_set.name);
      } else {
        snprintf(name, sizeof(name), "pin16-%02X%02X", addr.addr[1], addr.addr[0]);
      }
      snprintf(od_nm, sizeof(od_nm), "OD%02X%02X%02X", addr.addr[2], addr.addr[1], addr.addr[0]);   // OpenDisplay name: OD + the end of the address
      sl_bt_gatt_server_write_attribute_value(gattdb_device_name, 0, strlen(name), (uint8_t *)name);
      {
        sl_status_t sc = sl_bt_advertiser_create_set(&adv_set);
        if (sc != SL_STATUS_OK) ev_add(EV_ERR, 10, (uint16_t)sc);
        sc = sl_bt_legacy_advertiser_generate_data(adv_set, sl_bt_advertiser_general_discoverable);
        if (sc != SL_STATUS_OK) ev_add(EV_ERR, 11, (uint16_t)sc);
      }
      stack_ready = true;
      led_init();
      led_set_amber(g_set.amber_g);
      ev_add(EV_BOOT, (uint16_t)sysinfo_reset_cause(), BG_FW_VERSION);
      slot_show_arm();
      bthome_init(name);
      {
        // After a power-up, a battery change or a restart command the panel shows the splash first, the window opens afterwards.
        // Not after a watchdog, lock-up or brown-out reset: such a loop must not wear out the battery with panel refreshes.
        uint32_t rc = sysinfo_reset_cause();
        bool bad = (rc & (EMU_RSTCAUSE_WDOG0 | EMU_RSTCAUSE_LOCKUP | EMU_RSTCAUSE_DVDDBOD | EMU_RSTCAUSE_DVDDLEBOD
                          | EMU_RSTCAUSE_DECBOD | EMU_RSTCAUSE_AVDDBOD)) != 0;
        if (bad && g_set.led >= 1) led_flash(LC_RED, 2, 100, 150);
        else if (g_set.opmode == 1) { if (g_set.led >= 2) led_flash(LC_CYAN, 3, 60, 120); }
        else led_event(LC_WHITE, 60, 2);
        beacon_boot(bad);                   // a kept beacon starts again, but not after a crash or low voltage
        if (!g_set.splash_off && !bad && slot_request_pic(PIC_SPLASH, true)) {
          ev_add(EV_WIN, 0xFFFF, 0);        // splash first
        } else {
          open_window();                    // window right after power-up
        }
      }
      break;
    }
    case sl_bt_evt_connection_opened_id:
      conn = evt->data.evt_connection_opened.connection;
      ev_add(EV_OPEN, conn, 0);
      conn_open_ms = now_ms();
      got_rx = false;
      close_req_ms = 0;
      advertising = false;
      bthome_quiet();
      notify_on = false;
      od_via = false;
      od_notify = false;
      att_mtu = 23;
      q_cnt = 0;
      last_activity = now_ms();
      led_event(LC_GREEN, 40, 2);
      break;
    case sl_bt_evt_connection_closed_id: {
      conn = NO_CONN;
      od_link_down();
      slot_link_closed();
      ota_link_closed();
      q_cnt = 0;
      od_via = false;
      od_notify = false;
      last_close_reason = evt->data.evt_connection_closed.reason;
      if (led_kind == 1) { led_stop(); led_kind = 0; }
      ev_add(EV_CLOSE, last_close_reason, 0);
      // Diagnostics: red flashes = why the link ended
      // 1 supervision timeout, 2 failed to establish, 3 remote user terminated, 4 local host, 5 other
      uint8_t code = (uint8_t)(last_close_reason & 0xFF);
      uint8_t n = (code == 0x08) ? 1 : (code == 0x3E) ? 2 : (code == 0x13) ? 3 : (code == 0x16) ? 4 : 5;
      notify_on = false;
      // After a link ends the window is open again for BLE_AFTER_MS, so a reconnect works at any time
      // (the first window may have run out while the phone was connected). Advertising first, flashes after.
      if (close_win_req) {
        close_win_req = false;
        close_window();
      } else {
        open_window_ms((uint32_t)g_set.wt * 1000UL);
        if (g_set.led >= 3) led_flash(LC_RED, n, 60, 120);
      }
      break;
    }
    case sl_bt_evt_gatt_mtu_exchanged_id:
      att_mtu = evt->data.evt_gatt_mtu_exchanged.mtu;
      ev_add(EV_MTU, att_mtu, 0);
      break;
    case sl_bt_evt_connection_parameters_id: {
      uint16_t itv = evt->data.evt_connection_parameters.interval;     // units of 1.25 ms
      uint16_t lat = evt->data.evt_connection_parameters.latency;
      ev_add(EV_PARAM, itv, lat);
      ev_add(EV_PARAM, evt->data.evt_connection_parameters.timeout, evt->data.evt_connection_parameters.security_mode);
      break;
    }
    case sl_bt_evt_advertiser_timeout_id:
      if (evt->data.evt_advertiser_timeout.handle == adv_set) {
        ev_add(EV_ADVTMO, 0, 0);
        advertising = false;
      } else {
        bthome_on_timeout(evt->data.evt_advertiser_timeout.handle);
      }
      break;
    case sl_bt_evt_gatt_server_characteristic_status_id:
      if (evt->data.evt_gatt_server_characteristic_status.characteristic == gattdb_od_char
          && evt->data.evt_gatt_server_characteristic_status.status_flags == sl_bt_gatt_server_client_config) {
        od_via = true;                                // the client uses the OpenDisplay service
        od_notify = (evt->data.evt_gatt_server_characteristic_status.client_config_flags & sl_bt_gatt_notification) != 0;
        ev_add(EV_CHAR, (uint16_t)(0x100 | od_notify), 0);
        conn_open_ms = now_ms();
        last_activity = conn_open_ms;
        break;
      }
      if (evt->data.evt_gatt_server_characteristic_status.characteristic == gattdb_tx
          && evt->data.evt_gatt_server_characteristic_status.status_flags == sl_bt_gatt_server_client_config) {
        notify_on = (evt->data.evt_gatt_server_characteristic_status.client_config_flags
                     & sl_bt_gatt_notification) != 0;
        ev_add(EV_CHAR, notify_on, 0);
        if (notify_on) {
          conn_open_ms = now_ms();               // the client is alive: restart the first-command timer
          last_activity = conn_open_ms;
          ble_badge_say("READY");                 // lets the page see that the tag got the descriptor write
        }
        if (conn != NO_CONN) {
          int8_t r = 0;
          if (sl_bt_connection_get_median_rssi(conn, &r) == SL_STATUS_OK) {
            ev_add(EV_RSSI, (uint16_t)(int16_t)r, 0);
          }
        }
      }
      break;
    case sl_bt_evt_gatt_server_attribute_value_id:
      if (evt->data.evt_gatt_server_attribute_value.attribute == gattdb_od_char) {
        od_via = true;
        last_activity = now_ms();
        last_xchg = last_activity;
        got_rx = true;
        ev_add(EV_RX, (uint16_t)evt->data.evt_gatt_server_attribute_value.value.len, evt->data.evt_gatt_server_attribute_value.value.len ? evt->data.evt_gatt_server_attribute_value.value.data[0] : 0);
        if (evt->data.evt_gatt_server_attribute_value.value.len >= 2 && evt->data.evt_gatt_server_attribute_value.value.data[0] == 0x00) {
          od_rx(evt->data.evt_gatt_server_attribute_value.value.data, (uint8_t)evt->data.evt_gatt_server_attribute_value.value.len);
        }
        break;
      }
      if (evt->data.evt_gatt_server_attribute_value.attribute == gattdb_rx) {
        last_activity = now_ms();
        // keep-alive pings (one byte 01) are logged only as the first command of a link, otherwise they flood the ring
        if (!got_rx || !(evt->data.evt_gatt_server_attribute_value.value.len == 1 && evt->data.evt_gatt_server_attribute_value.value.data[0] == 1)) {
          ev_add(EV_RX, (uint16_t)evt->data.evt_gatt_server_attribute_value.value.len, evt->data.evt_gatt_server_attribute_value.value.data[0]);
        }
        if (!(evt->data.evt_gatt_server_attribute_value.value.len == 1 && evt->data.evt_gatt_server_attribute_value.value.data[0] == 1)) {
          last_xchg = now_ms();
          if (led_kind != 2 && !led_pulsing() && g_set.led >= 2) { led_pulse(LC_GREEN, 1600, 200); led_kind = 1; }
        }
        got_rx = true;
        handle_command(evt->data.evt_gatt_server_attribute_value.value.data,
                       (uint8_t)evt->data.evt_gatt_server_attribute_value.value.len);
      }
      break;
    default:
      break;
  }
}
