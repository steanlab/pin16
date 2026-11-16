// pin16: LED engine, see led.h. Own code.
#include <stdint.h>
#include <stdbool.h>
#include "em_cmu.h"
#include "em_gpio.h"
#include "em_timer.h"
#include "em_core.h"
#include "sl_sleeptimer.h"
#include "sl_power_manager.h"
#include "hw.h"
#include "led.h"

// The three LED pins are active low. hw_pins.led[] is in the order red, green, blue as the eye sees them (PC5, PC6, PC7; found by
// looking at the tag). Channel 0 = red, 1 = green, 2 = blue.
#define TOP        999            // 38.4 MHz / 128 / 1000 = 300 Hz PWM
#define STEP_MS    20             // pulse update step

static const uint8_t COL[8][3] = {  // full-scale mix per colour, tune by eye (amber: red strong, green weaker)
  {0, 0, 0},      // off
  {0, 255, 0},    // green
  {0, 0, 255},    // blue
  {255, 0, 0},    // red
  {255, 150, 0},  // amber (green part is adjustable, led_set_amber)
  {0, 200, 255},  // cyan
  {255, 0, 255},  // magenta
  {255, 150, 255} // white
};

static uint8_t  pin[3];                  // port<<4 | pin per channel, 0 = absent
static bool     ok = false, hw_on = false, em1 = false;
static uint8_t  cur[3];
static sl_sleeptimer_timer_handle_t tmr;
enum { M_NONE, M_FLASH, M_PULSE, M_HOLD };
static uint8_t  mode = M_NONE, col = 0, left = 0, peak = 255;
static uint16_t on_ms = 0, gap_ms = 0, period = 1600, t_ms = 0;
static bool     lit = false, bg = false;     // bg: background pulse (beacon), any other pattern replaces it and does not count as busy

static void hw_start(void)
{
  if (hw_on) return;
  CMU_ClockEnable(cmuClock_TIMER1, true);
  TIMER_Init_TypeDef ti = TIMER_INIT_DEFAULT;
  ti.enable = false;
  ti.prescale = timerPrescale128;
  ti.mode = timerModeUp;
  TIMER_Init(TIMER1, &ti);
  TIMER_TopSet(TIMER1, TOP);
  TIMER_InitCC_TypeDef cc = TIMER_INITCC_DEFAULT;
  cc.mode = timerCCModePWM;
  cc.cofoa = timerOutputActionSet;       // high at the start of a period, cleared at the match; inverted: low = LED lit
  cc.cmoa = timerOutputActionClear;
  cc.outInvert = true;
  for (int c = 0; c < 3; c++) TIMER_InitCC(TIMER1, (unsigned)c, &cc);
  TIMER_CounterSet(TIMER1, 0);
  TIMER_Enable(TIMER1, true);
  sl_power_manager_add_em_requirement(SL_POWER_MANAGER_EM1);   // the timer stops in EM2
  em1 = true;
  hw_on = true;
}

static void hw_stop(void)
{
  if (!hw_on) return;
  TIMER_Enable(TIMER1, false);
  GPIO->TIMERROUTE[1].ROUTEEN = 0;
  for (int c = 0; c < 3; c++) if (pin[c]) GPIO_PinOutSet((GPIO_Port_TypeDef)((pin[c] & 0x70) >> 4), pin[c] & 0x0F);
  CMU_ClockEnable(cmuClock_TIMER1, false);
  if (em1) { sl_power_manager_remove_em_requirement(SL_POWER_MANAGER_EM1); em1 = false; }
  hw_on = false;
}

static void put(const uint8_t v[3])      // levels 0..255 per channel, gamma 2
{
  if (!ok) return;
  if (!(v[0] | v[1] | v[2])) { hw_stop(); cur[0] = cur[1] = cur[2] = 0; return; }
  hw_start();
  uint32_t en = 0;
  volatile uint32_t *rt[3] = { &GPIO->TIMERROUTE[1].CC0ROUTE, &GPIO->TIMERROUTE[1].CC1ROUTE, &GPIO->TIMERROUTE[1].CC2ROUTE };
  for (int c = 0; c < 3; c++) {
    cur[c] = v[c];
    if (!pin[c] || !v[c]) {
      if (pin[c]) GPIO_PinOutSet((GPIO_Port_TypeDef)((pin[c] & 0x70) >> 4), pin[c] & 0x0F);
      continue;
    }
    uint32_t d = ((uint32_t)v[c] * v[c]) / 65;          // 0..1000
    if (d < 1) d = 1;
    TIMER_CompareSet(TIMER1, (unsigned)c, d);
    *rt[c] = ((uint32_t)((pin[c] & 0x70) >> 4) << _GPIO_TIMER_CC0ROUTE_PORT_SHIFT) | ((uint32_t)(pin[c] & 0x0F) << _GPIO_TIMER_CC0ROUTE_PIN_SHIFT);
    en |= (1UL << c);
  }
  GPIO->TIMERROUTE[1].ROUTEEN = en;
}

static uint8_t amber_g = 150;

static void put_col(uint8_t c, uint8_t level)    // colour c scaled by level 0..255
{
  uint8_t v[3];
  for (int i = 0; i < 3; i++) v[i] = (uint8_t)(((uint32_t)COL[c & 7][i] * level) / 255);
  if ((c & 7) == LC_AMBER) v[1] = (uint8_t)(((uint32_t)amber_g * level) / 255);
  put(v);
}

static void cb(sl_sleeptimer_timer_handle_t *h, void *d)
{
  (void)h; (void)d;
  if (mode == M_FLASH) {
    if (lit) {
      put_col(LC_OFF, 0);
      lit = false;
      if (left > 0) left--;
      if (left > 0) sl_sleeptimer_start_timer_ms(&tmr, gap_ms ? gap_ms : 1, cb, NULL, 0, 0);
      else mode = M_NONE;
    } else if (left > 0) {
      put_col(col, peak);
      lit = true;
      sl_sleeptimer_start_timer_ms(&tmr, on_ms, cb, NULL, 0, 0);
    }
  } else if (mode == M_PULSE) {
    t_ms += STEP_MS;
    if (t_ms >= period) t_ms -= period;
    uint16_t half = period / 2, x = t_ms < half ? t_ms : (uint16_t)(period - t_ms);
    uint32_t lvl = (uint32_t)x * peak / half;
    put_col(col, (uint8_t)lvl);
  } else if (mode == M_HOLD) {
    put_col(LC_OFF, 0);
    mode = M_NONE;
  }
}

static uint8_t hv(const hw_pin_t *g)                    // port<<4 | pin with bit 7 set, 0 = no such LED
{
  return g->port == HW_NONE ? 0 : (uint8_t)(0x80 | (g->port << 4) | g->pin);
}

void led_init(void)
{
  ok = false;
  pin[0] = hv(&hw_pins.led[0]);
  pin[1] = hv(&hw_pins.led[1]);
  pin[2] = hv(&hw_pins.led[2]);
  ok = (pin[0] | pin[1] | pin[2]) != 0;
}

void led_stop(void)
{
  CORE_DECLARE_IRQ_STATE;
  CORE_ENTER_ATOMIC();
  sl_sleeptimer_stop_timer(&tmr);
  mode = M_NONE; lit = false; left = 0;
  put_col(LC_OFF, 0);
  CORE_EXIT_ATOMIC();
}

void led_flash_dim(uint8_t c, uint8_t n, uint16_t on, uint16_t gap, uint8_t pk)
{
  if (n == 0 || !ok) return;
  CORE_DECLARE_IRQ_STATE;
  CORE_ENTER_ATOMIC();
  sl_sleeptimer_stop_timer(&tmr);
  mode = M_FLASH; col = c; left = n; on_ms = on; gap_ms = gap; peak = pk; lit = true;
  put_col(col, peak);
  sl_sleeptimer_start_timer_ms(&tmr, on_ms, cb, NULL, 0, 0);
  CORE_EXIT_ATOMIC();
}

void led_flash(uint8_t c, uint8_t n, uint16_t on, uint16_t gap) { led_flash_dim(c, n, on, gap, 255); }

static void pulse_start(uint8_t c, uint16_t per, uint8_t pk, bool back)
{
  if (!ok || per < 4 * STEP_MS) return;
  CORE_DECLARE_IRQ_STATE;
  CORE_ENTER_ATOMIC();
  sl_sleeptimer_stop_timer(&tmr);
  mode = M_PULSE; col = c; period = per; peak = pk; t_ms = 0; lit = false; bg = back;
  put_col(LC_OFF, 0);
  sl_sleeptimer_start_periodic_timer_ms(&tmr, STEP_MS, cb, NULL, 0, 0);
  CORE_EXIT_ATOMIC();
}

void led_pulse(uint8_t c, uint16_t per, uint8_t pk) { pulse_start(c, per, pk, false); }
void led_pulse_bg(uint8_t c, uint16_t per, uint8_t pk) { pulse_start(c, per, pk, true); }

bool led_pulsing(void) { return mode == M_PULSE && !bg; }
bool led_busy(void) { return mode != M_NONE && !(mode == M_PULSE && bg); }
bool led_bg_running(void) { return mode == M_PULSE && bg; }
void led_set_amber(uint8_t g) { amber_g = g; }
uint8_t led_get_amber(void) { return amber_g; }

void led_rgb(uint8_t r, uint8_t g, uint8_t b, uint16_t ms)
{
  if (!ok) return;
  CORE_DECLARE_IRQ_STATE;
  CORE_ENTER_ATOMIC();
  sl_sleeptimer_stop_timer(&tmr);
  uint8_t v[3] = { r, g, b };
  put(v);
  mode = M_HOLD; lit = false;
  if (ms) sl_sleeptimer_start_timer_ms(&tmr, ms, cb, NULL, 0, 0);
  CORE_EXIT_ATOMIC();
}
