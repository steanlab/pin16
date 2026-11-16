// pin16: settings commands, same numbers as badge437. Answers are text lines. Own code.
#include <stdio.h>
#include <string.h>
#include "cmd_set.h"
#include "settings.h"
#include "slot.h"
#include "ble_badge.h"
#include "hw.h"
#include "nfc.h"
#include "tapgate.h"
#include "draw.h"
#include "panel.h"
#include "em_device.h"
#include "sysinfo.h"
#include "hwcard.h"
#include "bthome.h"
#include "pics.h"
#include "led.h"
#include "fwver.h"
#include "beacon.h"
#include "btl_interface.h"


bool cmdset_is(uint8_t c) { return c == 0x4A || c == 0x4B || c == 0x4D || c == 0x4E || c == 0x4F; }

static void say_ss(void)
{
  ble_badge_say("SS en=%u every=%u mask=%08lX poll=%u clean=%u/%u cur=%d next=%d", g_set.ss_en, g_set.ss_every,
                (unsigned long)g_set.ss_mask, g_set.ss_order, g_set.clean_n, g_set.clean_mode, slot_current(), slot_next_preview());
}

static void say_bc(void)
{
  uint8_t p = beacon_active() ? beacon_preset() : g_set.bc_preset;
  ble_badge_say("BEACON on=%u preset=%u col=%u lvl=%u dur=%u keep=%u left=%lu mv=%u", beacon_active() ? 1 : 0, p, g_set.bc_col, g_set.bc_lvl,
                g_set.bc_dur, g_set.bc_keep, (unsigned long)beacon_left_s(), sysinfo_vbat());
}

// Commands that change how the tag can be reached, or that rewrite the NFC chip, need a recent touch (tapgate.h).
static bool need_tap(void)
{
  if (tapgate_open()) return true;
  ble_badge_say("ERR tap: touch the tag with the phone, repeat");
  return false;
}

static void save_ok(void)
{
  if (!settings_save()) ble_badge_say("ERR save (flash busy)");
}

static void nfc_say_result(nfc_status_t st)
{
  ble_badge_say(st == NFC_OK ? "NFC written" : "NFC ERR %d (1 bus, 2 argument or too big for the chip, 3 verify, 4 busy)", (int)st);
}

static void nfc_say_info(void)
{
  const nfc_info_t *ni = nfc_info();
  ble_badge_say("NFC present=%u r25=%u len=%u cap=%u kind=%u", ni->present ? 1 : 0, ni->reg25, ni->ndef_len, ni->cap, ni->kind);
  size_t cut = strlen(ni->text);
  if (cut > 80) {                               // split between two UTF-8 characters, never inside one
    cut = 80;
    while (cut > 0 && ((uint8_t)ni->text[cut] & 0xC0) == 0x80) cut--;
  }
  ble_badge_say("NFC text=%.*s", (int)cut, ni->text);
  if (ni->text[cut]) ble_badge_say("NFC text+=%.40s", ni->text + cut);
}

void cmdset_run(const uint8_t *d, uint8_t len)
{
  switch (d[0]) {
    case 0x4A:                                  // en every16 mask0 poll [clean_n clean_mode [mask1 [mask2 [mask3]]]]
      if (len < 6) { ble_badge_say("ERR short"); return; }
      {
        uint16_t every = (uint16_t)(d[2] | (d[3] << 8));
        if (every < 1 || every > 1080) { ble_badge_say("ERR every"); return; }
        g_set.ss_en = d[1] ? 1 : 0;
        g_set.ss_every = every;
        uint32_t m = d[4];
        if (len > 8) m |= (uint32_t)d[8] << 8;
        if (len > 9) m |= (uint32_t)d[9] << 16;
        if (len > 10) m |= (uint32_t)d[10] << 24;
        g_set.ss_mask = m;
        g_set.ss_order = d[5] ? 1 : 0;
        if (len > 7) { g_set.clean_n = d[6]; g_set.clean_mode = d[7]; }
        save_ok();
        slot_show_arm();
        say_ss();
      }
      break;
    case 0x4B:
      say_ss();
      break;
    case 0x4D:                                  // mode slot [lut]
      if (len < 3) { ble_badge_say("ERR short"); return; }
      if (d[1] > 3 || d[2] > 31) { ble_badge_say("ERR range"); return; }
      g_set.orient = d[1];
      g_set.od_slot = d[2];
      if (len > 3) g_set.lut = d[3];
      save_ok();
      ble_badge_say("OD mode=%u slot=%u lut=%02X", g_set.orient, g_set.od_slot, g_set.lut);
      break;
    case 0x4E: {                                // name tail 1..5 printable characters, empty = default
      if (!need_tap()) return;
      uint8_t n = len > 1 ? (uint8_t)(len - 1) : 0;
      if (n > 5) { ble_badge_say("ERR name"); return; }
      for (uint8_t i = 0; i < n; i++) if (d[1 + i] < 0x21 || d[1 + i] > 0x7E) { ble_badge_say("ERR name"); return; }
      memset(g_set.name, 0, sizeof(g_set.name));
      memcpy(g_set.name, d + 1, n);
      save_ok();
      ble_badge_say("NAME saved, reboot (4F A5) to use it");
      break;
    }
    case 0x4F:
      if (len < 2) { ble_badge_say("ERR short"); return; }
      switch (d[1]) {
        case 0xA5: ble_badge_say("REBOOT"); ble_badge_reboot(); break;
        case 0x01: if (len < 3 || d[2] > 5) { ble_badge_say("ERR range"); return; } g_set.txp = d[2]; save_ok(); ble_badge_apply_tx(); ble_badge_say("TX %u", g_set.txp); break;
        case 0x02: ble_badge_say("WD on 131s rc=%04lX", (unsigned long)sysinfo_reset_cause()); break;
        case 0x03: {
          uint16_t mv = sysinfo_vbat();
          int8_t tc = hw_temperature();
          ble_badge_say("SYS tx=%u wd=1 led=%u v=%u mode=%u wt=%u wb=%u tap=%u sw=%lu ct=%lu tc=%d", g_set.txp, g_set.led, mv, g_set.opmode,
                        g_set.wt, g_set.wb, g_set.tap_next, (unsigned long)settings_seq(),
                        (unsigned long)((HFXO0->XTALCTRL & _HFXO_XTALCTRL_CTUNEXIANA_MASK) >> _HFXO_XTALCTRL_CTUNEXIANA_SHIFT), (int)tc);
          break;
        }
        case 0x04: if (len < 3 || d[2] > 3) { ble_badge_say("ERR range"); return; } g_set.led = d[2]; save_ok(); ble_badge_say("LED %u", g_set.led); break;
        case 0x05: if (!need_tap()) return; if (len < 3 || d[2] > 1) { ble_badge_say("ERR range"); return; } g_set.opmode = d[2]; save_ok(); ble_badge_say("MODE %u, restarting", g_set.opmode); ble_badge_reboot(); break;
        case 0x06:
          if (!need_tap()) return;
          if (len < 4 || d[2] < 10 || d[2] > 250 || d[3] < 10 || d[3] > 250) { ble_badge_say("ERR range"); return; }
          g_set.wt = d[2]; g_set.wb = d[3]; save_ok(); ble_badge_say("WINDOWS tap=%u boot=%u", g_set.wt, g_set.wb);
          break;
        case 0x07: ble_badge_say("OTA: command 46 (46 00 = status)"); break;
        case 0x16:                              // beacon / locator: [preset col lvl dur keep]; preset 0 = off; without arguments only reads
          if (len >= 7) {
            uint8_t p = d[2];
            if (p > 5 || d[3] < 1 || d[3] > 7 || d[4] < 1 || d[4] > 4 || d[5] > 4 || d[6] > 1) { ble_badge_say("ERR range"); return; }
            if (p == 0) {
              beacon_stop();
              g_set.bc_preset = 0; g_set.bc_keep = 0;
            } else {
              uint8_t lvl = d[4] > beacon_max_lvl(p) ? beacon_max_lvl(p) : d[4];
              uint8_t dur = d[5] > beacon_max_dur(p) ? beacon_max_dur(p) : d[5];
              if (!beacon_start(p, d[3], lvl, dur)) { ble_badge_say("ERR battery low or no LED"); return; }
              g_set.bc_preset = p; g_set.bc_col = d[3]; g_set.bc_lvl = lvl; g_set.bc_dur = dur; g_set.bc_keep = d[6];
            }
            save_ok();
          }
          say_bc();
          break;
        case 0x17:                              // amber green part 0..255 [g]; without arguments only reads
          if (len >= 3) { g_set.amber_g = d[2]; led_set_amber(d[2]); save_ok(); }
          ble_badge_say("AMBER g=%u", led_get_amber());
          break;
        case 0x09: ble_badge_dump_events(); break;
        case 0x0A: if (len > 2 && d[2]) ble_badge_close_link_and_window(); else ble_badge_close_link(); break;
        case 0x14:                              // LED test: [r g b [ms/10]] steady colour, default 2 s; without arguments off
          if (len >= 5) led_rgb(d[2], d[3], d[4], len >= 6 ? (uint16_t)d[5] * 10u : 2000u); else led_stop();
          ble_badge_say("LEDT");
          break;
        case 0x15:                              // LED test: pulse [colour 1..7] until 4F 15 without arguments, or flash n times [colour, n]
          if (len >= 3 && d[2] >= 1 && d[2] <= 7) {
            if (len >= 4 && d[3] > 0) led_flash(d[2], d[3] > 9 ? 9 : d[3], 120, 150);
            else { led_pulse(d[2], 1600, 200); }
          } else led_stop();
          ble_badge_say("LEDP");
          break;
        case 0x13:                              // read slot back as binary frames (thumbnails): [slot]
          if (len < 3) { ble_badge_say("ERR short"); return; }
          if (!slot_dump_start(d[2])) ble_badge_say("DUMP n/a");
          break;
        case 0x10: {                            // experimental: temperature sensor of the panel (takes about one second)
          uint8_t b[3] = { 0, 0, 0 };
          if (draw_probe_temp(b)) {
            ble_badge_say("PT %dC", (int)(int8_t)b[0]);
          } else {
            ble_badge_say("PTEMP n/a");
          }
          break;
        }
        case 0x0C:                              // BTHome beacon: [en itv mask], without arguments only reads
          if (len >= 5) {
            if (d[2] > 1 || d[3] > 3 || (d[4] & 0x7F) == 0) { ble_badge_say("ERR range"); return; }
            g_set.bt_en = d[2]; g_set.bt_itv = d[3]; g_set.bt_mask = d[4] & 0x7F;
            if (len >= 6) g_set.bt_win = d[5] ? 1 : 0;
            save_ok();
            bthome_apply();
          }
          ble_badge_say("BT en=%u itv=%u mask=%02X win=%u", g_set.bt_en, g_set.bt_itv, g_set.bt_mask, g_set.bt_win);
          break;
        case 0x0D: {                            // chip ID and reset cause
          uint32_t hi, lo;
          sysinfo_uid(&hi, &lo);
          ble_badge_say("ID %08lX%08lX", (unsigned long)hi, (unsigned long)lo);
          ble_badge_say("RST rc=%04lX fw=%u", (unsigned long)sysinfo_reset_cause(), (unsigned)BG_FW_VERSION);
          {
            BootloaderInformation_t bi;
            bi.version = 0;
            bootloader_getInfo(&bi);
            // bi.version is the version of the bootloader main stage. The variant id (03 = Solum autodetect) is patched into the
            // version word of the bootloader's info struct (pointer at word 13 of the vector table at address 0), see bootloader/generate_variants.py
            uint32_t pv = *(volatile const uint32_t *)0x34, vid = 0;
            if (pv >= 0x40 && pv < 0x5FC0 && *(volatile const uint32_t *)pv == 0xFA79B713UL) vid = *(volatile const uint32_t *)(pv + 32);
            ble_badge_say("BTL main=%08lX props=%08lX id=%02X", (unsigned long)bi.version, (unsigned long)vid, (unsigned)(vid & 0xFF));
          }
          {
            uint32_t tot;
            uint32_t fr = sysinfo_stack_free(&tot);
            ble_badge_say("STK free=%lu of %lu", (unsigned long)fr, (unsigned long)tot);
          }
#ifdef BG_DIAG
          hwc_card_start();                     // diagnostic build: the hardware card follows every 4F 0D
#endif
          break;
        }
#ifdef BG_DIAG
        case 0x0F:                              // hardware card again; 4F 0F 00 clears the bus trace (the next panel draw is recorded)
          if (len > 2 && d[2] == 0) { hwc_clear(); ble_badge_say("TRACE cleared"); } else hwc_card_start();
          break;
#endif
        case 0x0E: {                            // battery: now, lowest during the last draw, lowest since power-up
          uint16_t mv = sysinfo_vbat();
          ble_badge_say("BAT mv=%u min=%u ever=%u pct=%u low=%u draws=%lu", mv, sysinfo_min_load(), sysinfo_min_ever(),
                        sysinfo_percent(mv), sysinfo_low(mv) ? 1 : 0, (unsigned long)sysinfo_draws());
          break;
        }
        case 0x11:                              // picture made by the firmware: 1 splash, 2 bars, 3 white, 4 black, 5 yellow, 6 red, 7 checker, 8 frame; 0x20 clean cycle
          if (len < 3) { ble_badge_say("ERR short"); return; }
          if (slot_busy()) { ble_badge_say("ERR busy"); return; }
          if (d[2] == 0x20) {
            ble_badge_say(slot_request_clean() ? "CLEAN start (about 2 minutes)" : "ERR busy");
          } else if (pic_valid(d[2])) {
            if (!slot_request_pic(d[2], false)) ble_badge_say("ERR busy");
          } else {
            ble_badge_say("ERR range");
          }
          break;
        case 0x12:                              // boot splash [off]: 0 = on, 1 = off
          if (len >= 3) {
            if (d[2] > 1) { ble_badge_say("ERR range"); return; }
            g_set.splash_off = d[2];
            save_ok();
          }
          ble_badge_say("SPLASH off=%u", g_set.splash_off);
          break;
        case 0x18:                              // NFC chip: without arguments reads, with a URL (printable ASCII) rewrites the record
          if (len >= 3) {
            if (!need_tap()) return;
            char url[NFC_URL_MAX + 1];
            uint8_t n = (uint8_t)(len - 2);
            if (n > NFC_URL_MAX) { ble_badge_say("ERR url too long (max %d)", NFC_URL_MAX); return; }
            memcpy(url, d + 2, n);
            url[n] = 0;
            nfc_say_result(nfc_write(NDEF_URI, url, NULL, 0));
          }
          nfc_say_info();
          break;
        case 0x1B: {                            // NFC chip: 1B 01 text (UTF-8) | 1B 02 auth ssid 00 key (Wi-Fi; auth 0 open, 1 WPA2, 2 WPA/WPA2) | 1B 00 url
          if (len < 4) { ble_badge_say("ERR short"); return; }
          if (!need_tap()) return;
          char a[NDEF_TEXT_MAX + 1], k[NDEF_KEY_MAX + 1];
          uint8_t kind = d[2];
          const uint8_t *p = d + 3;
          size_t n = (size_t)(len - 3);
          nfc_status_t st = NFC_E_ARG;
          if (kind == 0 || kind == 1) {
            if (n > NDEF_TEXT_MAX) { ble_badge_say("ERR too long (max %d)", NDEF_TEXT_MAX); return; }
            memcpy(a, p, n); a[n] = 0;
            st = nfc_write(kind == 0 ? NDEF_URI : NDEF_TEXT, a, NULL, 0);
          } else if (kind == 2) {
            size_t z = 1;                                   // d[3] = auth, then the network name, a 0 byte, the key
            while (z < n && p[z] != 0) z++;
            if (z >= n || z - 1 > NDEF_SSID_MAX || n - z - 1 > NDEF_KEY_MAX) { ble_badge_say("ERR wifi"); return; }
            memcpy(a, p + 1, z - 1); a[z - 1] = 0;
            memcpy(k, p + z + 1, n - z - 1); k[n - z - 1] = 0;
            st = nfc_write(NDEF_WIFI, a, k, p[0]);
            ndef_wipe(k, sizeof(k));
          } else {
            ble_badge_say("ERR range");
            return;
          }
          nfc_say_result(st);
          nfc_say_info();
          break;
        }
        case 0x1A: ble_badge_say("PANEL %s %ux%u mirror=%u ctrl=%02X tag=%02X", panel_name(), (unsigned)panel_w(), (unsigned)panel_h(), panel_mirror() ? 1 : 0,
                                 hw_userdata()->ctrl, hw_userdata()->tag); break;
        case 0x19: ble_badge_say("TAP open=%u left=%lus", tapgate_open() ? 1 : 0, (unsigned long)tapgate_left_s()); break;
        case 0x0B: ble_badge_say("CFG or=%u od=%u lut=%02X nm=%.5s", g_set.orient, g_set.od_slot, g_set.lut, g_set.name); break;
        case 0x08: if (len < 3 || d[2] > 1) { ble_badge_say("ERR range"); return; } g_set.tap_next = d[2]; save_ok(); ble_badge_say("TAP next=%u", g_set.tap_next); break;
        default: ble_badge_say("ERR cmd 4F %02X", d[1]); break;
      }
      break;
  }
}
