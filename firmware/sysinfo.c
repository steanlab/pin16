// pin16: system information, see sysinfo.h. Own code.
#include "sysinfo.h"
#include "beacon.h"
#include "em_device.h"
#include "em_cmu.h"
#include "em_iadc.h"
#include "em_rmu.h"
#include "em_wdog.h"
#include "sl_sleeptimer.h"

#define WD_KEEPALIVE_MS   30000UL      // the main loop feeds the watchdog at least this often
#define LOAD_PERIOD_MS    250UL        // supply sampling during a draw
#define LOAD_FEED_MAX_MS  90000UL      // the draw timer feeds the watchdog for at most this long (a stuck panel must still reset the tag)

static uint32_t rcause = 0;
static volatile bool wd_due = false;
static volatile bool adc_busy = false;
static sl_sleeptimer_timer_handle_t wd_timer;
static sl_sleeptimer_timer_handle_t load_timer;
static volatile uint16_t min_load = 0;          // last draw
static volatile uint16_t min_ever = 0;
static volatile uint16_t last_mv = 0;
static volatile uint32_t load_start_tick = 0;
static volatile bool load_on = false;
static uint32_t draws = 0;

// Stack watermark: the unused part of the stack is filled with a pattern at start-up; the first word that no longer holds it
// shows how deep the stack has gone since (command 4F 0D). The stack sits at the start of the RAM and grows downwards.
extern uint32_t __StackLimit, __StackTop;
#define STK_PAT 0xA5A5A5A5UL

static void stack_paint(void)
{
  uint32_t *p = &__StackLimit;
  uint32_t *sp = (uint32_t *)__get_MSP() - 32;       // stay clear of the live frames
  while (p < sp) *p++ = STK_PAT;
}

uint32_t sysinfo_stack_free(uint32_t *total)
{
  const uint32_t *p = &__StackLimit;
  const uint32_t *sp = (const uint32_t *)__get_MSP();
  while (p < sp && *p == STK_PAT) p++;
  if (total) *total = (uint32_t)((uintptr_t)&__StackTop - (uintptr_t)&__StackLimit);
  return (uint32_t)((uintptr_t)p - (uintptr_t)&__StackLimit);
}

void sysinfo_early(void)
{
  rcause = RMU_ResetCauseGet();
  stack_paint();
}

uint32_t sysinfo_reset_cause(void) { return rcause; }

void sysinfo_uid(uint32_t *hi, uint32_t *lo)
{
  *hi = DEVINFO->EUI64H;
  *lo = DEVINFO->EUI64L;
}

// ---- supply voltage: the internal IADC measures the supply against the 1.2 V bandgap ----
static uint16_t read_avdd(void)
{
  IADC_Init_t init = IADC_INIT_DEFAULT;
  IADC_AllConfigs_t initAllConfigs = IADC_ALLCONFIGS_DEFAULT;
  IADC_InitSingle_t initSingle = IADC_INITSINGLE_DEFAULT;
  IADC_SingleInput_t initSingleInput = IADC_SINGLEINPUT_DEFAULT;

  CMU_ClockEnable(cmuClock_IADC0, true);
  IADC_reset(IADC0);
  CMU_ClockSelectSet(cmuClock_IADCCLK, cmuSelect_FSRCO);
  init.warmup = iadcWarmupNormal;
  init.srcClkPrescale = IADC_calcSrcClkPrescale(IADC0, 20000000, 0);
  initAllConfigs.configs[0].reference = iadcCfgReferenceInt1V2;
  initAllConfigs.configs[0].vRef = 1210;
  initAllConfigs.configs[0].osrHighSpeed = iadcCfgOsrHighSpeed2x;
  initAllConfigs.configs[0].analogGain = iadcCfgAnalogGain1x;
  initAllConfigs.configs[0].adcClkPrescale = IADC_calcAdcClkPrescale(IADC0, 10000000, 0, iadcCfgModeNormal, init.srcClkPrescale);
  initSingleInput.posInput = iadcPosInputAvdd;
  initSingleInput.negInput = iadcNegInputGnd;
  IADC_init(IADC0, &init, &initAllConfigs);
  IADC_initSingle(IADC0, &initSingle, &initSingleInput);
  IADC_command(IADC0, iadcCmdStartSingle);
  uint32_t guard = 200000;                         // never wait forever
  while ((IADC0->STATUS & (_IADC_STATUS_CONVERTING_MASK | _IADC_STATUS_SINGLEFIFODV_MASK)) != IADC_STATUS_SINGLEFIFODV) {
    if (--guard == 0) {
      IADC_command(IADC0, iadcCmdStopSingle);
      IADC_reset(IADC0);
      CMU_ClockEnable(cmuClock_IADC0, false);
      return 0;
    }
  }
  uint32_t sample = IADC_pullSingleFifoResult(IADC0).data;
  IADC_command(IADC0, iadcCmdStopSingle);
  IADC_reset(IADC0);
  CMU_ClockEnable(cmuClock_IADC0, false);
  return (uint16_t)((sample * 4UL * 1200UL) / 4095UL);
}

uint16_t sysinfo_vbat(void)
{
  if (adc_busy) return last_mv;
  adc_busy = true;
  uint16_t mv = read_avdd();
  adc_busy = false;
  if (mv) last_mv = mv;
  return mv;
}

static void on_load_timer(sl_sleeptimer_timer_handle_t *h, void *d)
{
  (void)h; (void)d;
  uint32_t el = sl_sleeptimer_tick_to_ms(sl_sleeptimer_get_tick_count() - load_start_tick);
  if (el < LOAD_FEED_MAX_MS) {
    WDOGn_Feed(WDOG0);
  }
  if (!adc_busy) {
    adc_busy = true;
    uint16_t mv = read_avdd();
    adc_busy = false;
    if (mv) {
      last_mv = mv;
      if (min_load == 0 || mv < min_load) min_load = mv;
      if (min_ever == 0 || mv < min_ever) min_ever = mv;
    }
  }
}

void sysinfo_load_begin(void)
{
  if (load_on) return;
  beacon_hold(true);                              // the panel needs the cell: no beacon flashes meanwhile
  min_load = 0;
  load_start_tick = sl_sleeptimer_get_tick_count();
  load_on = true;
  WDOGn_Feed(WDOG0);
  on_load_timer(NULL, NULL);                      // first sample before the panel starts
  sl_sleeptimer_start_periodic_timer_ms(&load_timer, LOAD_PERIOD_MS, on_load_timer, NULL, 0, 0);
}

void sysinfo_load_end(void)
{
  if (!load_on) return;
  sl_sleeptimer_stop_timer(&load_timer);
  load_on = false;
  on_load_timer(NULL, NULL);                      // last sample right after the refresh
  WDOGn_Feed(WDOG0);
  beacon_hold(false);
}

uint16_t sysinfo_min_load(void) { return min_load; }
uint16_t sysinfo_min_ever(void) { return min_ever; }

uint8_t sysinfo_percent(uint16_t mv)
{
  // CR2450, light load: rough curve (mV, percent)
  static const uint16_t v[] = { 2200, 2400, 2500, 2600, 2700, 2800, 2850, 2900, 2950 };
  static const uint8_t  p[] = {    0,    3,    6,   12,   25,   50,   75,   90,  100 };
  if (mv <= v[0]) return 0;
  if (mv >= v[8]) return 100;
  for (int i = 1; i < 9; i++) {
    if (mv <= v[i]) {
      return (uint8_t)(p[i - 1] + ((uint32_t)(mv - v[i - 1]) * (uint32_t)(p[i] - p[i - 1])) / (uint32_t)(v[i] - v[i - 1]));
    }
  }
  return 100;
}

bool sysinfo_low(uint16_t mv) { return mv != 0 && mv < 2500; }

uint32_t sysinfo_draws(void) { return draws; }
void sysinfo_draw_done(void) { draws++; }

// ---- watchdog ----
static void on_wd_timer(sl_sleeptimer_timer_handle_t *h, void *d)
{
  (void)h; (void)d;
  wd_due = true;                                   // the main loop feeds it: this proves that the main loop still runs
}

bool sysinfo_pending(void) { return wd_due; }

void sysinfo_poll(void)
{
  if (wd_due) {
    wd_due = false;
    WDOGn_Feed(WDOG0);
  }
}

void sysinfo_start(void)
{
  CMU_ClockEnable(cmuClock_WDOG0, true);
  CMU_ClockSelectSet(cmuClock_WDOG0, cmuSelect_ULFRCO);
  WDOG_Init_TypeDef w = WDOG_INIT_DEFAULT;
  w.perSel = wdogPeriod_128k;                      // about 131 s on the 1 kHz ULFRCO
  w.em2Run = true;
  w.em3Run = true;
#if defined(_WDOG_CFG_EM1RUN_MASK)
  w.em1Run = true;
#endif
  w.debugRun = false;
  WDOGn_Init(WDOG0, &w);
  sl_sleeptimer_start_periodic_timer_ms(&wd_timer, WD_KEEPALIVE_MS, on_wd_timer, NULL, 0, 0);
}
