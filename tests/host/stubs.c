#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "ble_badge.h"
#include "sysinfo.h"
#include "slot.h"
#include "pics.h"
#include "fake.h"
uint16_t stub_vbat = 3000, stub_mtu = 247; int stub_slot_busy = 0, stub_draw_pending = 0, stub_reboot_calls = 0, stub_hold_calls = 0;
char say_last[96]; int say_count = 0; char say_log[256][96]; int say_log_n = 0;
void ble_badge_say(const char *fmt, ...)
{
  va_list ap; va_start(ap, fmt); char b[256]; vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
  if (strlen(b) > 95) { fprintf(stderr, "ble_badge_say line longer than 95: %s\n", b); }
  strncpy(say_last, b, sizeof say_last - 1); say_last[sizeof say_last - 1] = 0; say_count++;
  strncpy(say_log[say_log_n % 256], say_last, 95); say_log[say_log_n % 256][95] = 0; say_log_n++;
}
void ble_badge_send(const uint8_t *p, uint8_t n) { (void)p; (void)n; }
bool ble_badge_room(void) { return true; }
bool ble_badge_link_ready(void) { return true; }
void ble_badge_led_work(uint8_t w) { (void)w; }
void ble_badge_touch(void) {}
void ble_badge_reboot_install(void) { stub_reboot_calls++; }
uint16_t ble_badge_mtu(void) { return stub_mtu; }
void ble_badge_request_window(void) {}
void ble_badge_request_boot_window(void) {}
bool ble_badge_is_active(void) { return false; }
uint16_t sysinfo_vbat(void) { return stub_vbat; }
// drawing: by default a draw "succeeds" at once (the slide tests do not care what is drawn); draw_real = 1 runs the real draw.c
// (compiled as real_draw_slot / real_draw_pic) on the real panel.c with the recording panel_io of test_panel.c
int draw_real = 0, draw_slot_calls = 0, draw_pic_calls = 0;
bool real_draw_slot(uint32_t a, uint32_t l, uint8_t f, uint8_t o, uint8_t lut);
bool real_draw_pic(uint8_t k, uint8_t o);
bool draw_slot(uint32_t a, uint32_t l, uint8_t f, uint8_t o, uint8_t lut) { draw_slot_calls++; return draw_real ? real_draw_slot(a, l, f, o, lut) : true; }
bool draw_pic(uint8_t k, uint8_t o) { draw_pic_calls++; return draw_real ? real_draw_pic(k, o) : true; }
void sysinfo_load_begin(void) {}
void sysinfo_load_end(void) {}
void sysinfo_draw_done(void) {}
