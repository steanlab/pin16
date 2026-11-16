// pin16: application glue: start-up order, taps and buttons, and the answers to the power manager. Own code.
#include <stdbool.h>
#include "app.h"
#include "hw.h"
#include "panel.h"
#include "draw.h"
#include "ble_badge.h"
#include "settings.h"
#include "slot.h"
#include "sysinfo.h"
#include "tapgate.h"
#include "sl_sleeptimer.h"
#include "sl_power_manager.h"
#include "em_cmu.h"

#define TAP_IGNORE_MS  2000

static volatile bool tap = false;
static uint64_t last_tap_tick = 0;      // 64-bit: the 32-bit tick counter wraps every 36 hours
static bool have_tap = false;

// Interrupt context: an NFC tap or a button edge. Every edge counts as a touch for the gate, the tag reacts once per 2 s.
static void on_input(uint8_t src)
{
  (void)src;
  tapgate_touch();
  uint64_t now = sl_sleeptimer_get_tick_count64();
  uint64_t since_ms = 0;
  sl_sleeptimer_tick64_to_ms(now - last_tap_tick, &since_ms);
  if (have_tap && since_ms < TAP_IGNORE_MS) {
    return;
  }
  have_tap = true;
  last_tap_tick = now;
  tap = true;
}

void app_init(void)
{
  sysinfo_early();                                // remember the reset cause before hw_init clears it
  // The tags have no 32 kHz crystal. The Bluetooth stack needs an accurate sleep clock between connection
  // events, so the LFRCO is put into precision mode (hardware re-calibrates it against the HFXO).
#if defined(PLFRCO_PRESENT)
  CMU_LFRCOSetPrecision(cmuPrecisionHigh);
#endif
  hw_init();
  panel_init();
  hw_inputs_start(on_input);
  settings_load();
  sysinfo_start();                                // watchdog: the main loop must feed it at least every 30 s
}

void app_process_action(void)
{
  if (tap) {
    tap = false;
    if (g_set.tap_next) {
      slot_request_next_tap();                    // the draw blocks the radio, so the window opens after it
    } else {
      ble_badge_request_window();
    }
  }
  ble_badge_process();
}

static bool event_pending(void)
{
  return tap || ble_badge_event_pending();
}

// Overrides of two hooks of the power manager (called from sl_power_manager_sleep). While a refresh runs, the waiting code
// sleeps by itself, so the manager is allowed to sleep then.
bool app_is_ok_to_sleep(void)
{
  if (draw_busy()) {
    return true;
  }
  return !event_pending();
}

sl_power_manager_on_isr_exit_t app_sleep_on_isr_exit(void)
{
  if (draw_busy() || event_pending()) {
    return SL_POWER_MANAGER_WAKEUP;
  }
  return SL_POWER_MANAGER_SLEEP;
}
