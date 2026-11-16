// pin16: beacon / locator, see beacon.h. Own code.
#include <stdint.h>
#include <stdbool.h>
#include "beacon.h"
#include "led.h"
#include "settings.h"
#include "sysinfo.h"
#include "sl_sleeptimer.h"

#define MV_START      2800          // do not start below this supply voltage
#define MV_STOP       2700          // stop below this (measured while the LED is dark)
#define MV_STOP_LOAD  2600          // stop below this (measured while the LED is lit: the cell sags)
#define CHECK_MS      30000UL

static const uint8_t  PEAK[4]   = { 60, 90, 120, 140 };            // PWM peak 0..255 for brightness 1..4 (gamma 2: about 6 / 15 / 22 / 30 % of full power)
static const uint16_t HOURS[5]  = { 1, 6, 24, 72, 168 };
static const uint8_t  RAINBOW[6] = { LC_RED, LC_AMBER, LC_GREEN, LC_CYAN, LC_BLUE, LC_MAGENTA };

static bool running = false, held = false;
static volatile bool chk_due = false;
static uint8_t preset = 0, col = 1, peak = 120;
static uint32_t n = 0;                                              // tick counter
static uint32_t end_ms = 0, last_chk = 0;
static sl_sleeptimer_timer_handle_t tmr;

uint8_t beacon_max_lvl(uint8_t p) { return p == BC_SOS || p == BC_BREATH ? 2 : (p == BC_RAINBOW ? 3 : 4); }
uint8_t beacon_max_dur(uint8_t p) { return p == BC_LOCATOR ? 4 : (p == BC_BREATH ? 1 : 2); }       // 7 days / 6 h / 24 h

static uint32_t now_ms(void)
{
  uint64_t ms = 0;
  sl_sleeptimer_tick64_to_ms(sl_sleeptimer_get_tick_count64(), &ms);
  return (uint32_t)ms;
}

static uint16_t tick_ms(uint8_t p)
{
  switch (p) {
    case BC_LOCATOR:   return 3000;
    case BC_HEARTBEAT: return 2500;
    case BC_RAINBOW:   return 1500;
    case BC_BREATH:    return 1000;
    default:           return 150;                                  // SOS: pattern grid
  }
}

static void step(void)                                              // sleeptimer interrupt
{
  if (!running || held || led_busy()) return;                       // an event or a panel draw has the LED
  switch (preset) {
    case BC_LOCATOR:   led_flash_dim(col, 1, 80, 0, peak); break;
    case BC_HEARTBEAT: led_flash_dim(col, 2, 70, 130, peak); break;
    case BC_RAINBOW:   led_flash_dim(RAINBOW[(n / 1) % 6], 1, 120, 0, peak); break;
    case BC_BREATH:    if (!led_bg_running()) led_pulse_bg(col, 4000, peak); break;
    case BC_SOS: {
      uint32_t k = n % 50;                                          // 7.5 s: S . . .   O - - -   S . . .   then a pause
      if (k == 0 || k == 21) led_flash_dim(col, 3, 150, 150, peak);
      else if (k == 7) led_flash_dim(col, 3, 450, 150, peak);
      break;
    }
    default: break;
  }
}

static void on_tick(sl_sleeptimer_timer_handle_t *h, void *d)
{
  (void)h; (void)d;
  n++;
  step();
  if (now_ms() - last_chk >= CHECK_MS) chk_due = true;
}

bool beacon_start(uint8_t p, uint8_t c, uint8_t lvl, uint8_t dur)
{
  beacon_stop();
  if (p < BC_LOCATOR || p > BC_BREATH) return false;
  uint16_t mv = sysinfo_vbat();
  if (mv && mv < MV_START) return false;
  if (c < 1 || c > 7) c = LC_GREEN;
  if (lvl < 1) lvl = 1;
  if (lvl > beacon_max_lvl(p)) lvl = beacon_max_lvl(p);
  if (dur > beacon_max_dur(p)) dur = beacon_max_dur(p);
  preset = p; col = c; peak = PEAK[lvl - 1]; n = 0; held = false; chk_due = false;
  last_chk = now_ms();
  end_ms = last_chk + (uint32_t)HOURS[dur] * 3600000UL;
  running = true;
  sl_sleeptimer_start_periodic_timer_ms(&tmr, tick_ms(p), on_tick, NULL, 0, 0);
  step();
  return true;
}

void beacon_stop(void)
{
  if (!running) return;
  running = false;
  sl_sleeptimer_stop_timer(&tmr);
  if (led_bg_running()) led_stop();
}

bool beacon_active(void) { return running; }
uint8_t beacon_preset(void) { return running ? preset : 0; }

uint32_t beacon_left_s(void)
{
  if (!running) return 0;
  int32_t d = (int32_t)(end_ms - now_ms());
  return d > 0 ? (uint32_t)d / 1000UL : 0;
}

void beacon_hold(bool hold) { held = hold; }

bool beacon_pending(void) { return chk_due; }

void beacon_poll(void)
{
  if (!chk_due) return;
  chk_due = false;
  last_chk = now_ms();
  if (!running) return;
  if ((int32_t)(now_ms() - end_ms) >= 0) { beacon_stop(); return; }              // time is over
  if (held) return;
  bool lit = led_busy() || led_bg_running();
  uint16_t mv = sysinfo_vbat();
  if (mv && mv < (lit ? MV_STOP_LOAD : MV_STOP)) beacon_stop();                // battery guard: stays off until restarted by hand
}

void beacon_boot(bool bad_reset)
{
  if (bad_reset || !g_set.bc_keep || g_set.bc_preset < BC_LOCATOR) return;
  beacon_start(g_set.bc_preset, g_set.bc_col, g_set.bc_lvl, g_set.bc_dur);
}
