// host fakes: virtual time, power manager, atomic counter, NOR flash simulator on the SPI driver interface
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "sl_sleeptimer.h"
#include "sl_udelay.h"
#include "em_core.h"
#include "sl_power_manager.h"
#include "em_cmu.h"
#include "em_gpio.h"
#include "em_timer.h"
#include "spidrv.h"
#include "hw.h"
#include "btl_interface.h"
#include "fake.h"

// ---------------- time ----------------
static uint64_t now_us = 0;
#define MAXT 64
static sl_sleeptimer_timer_handle_t *reg[MAXT];
static int nreg = 0;
int vt_start_errors = 0;
uint64_t vt_now_us(void) { return now_us; }
static void reg_add(sl_sleeptimer_timer_handle_t *h) { for (int i = 0; i < nreg; i++) if (reg[i] == h) return; if (nreg >= MAXT) { fprintf(stderr, "too many timers\n"); abort(); } reg[nreg++] = h; }
int vt_running_timers(void) { int n = 0; for (int i = 0; i < nreg; i++) if (reg[i]->run) n++; return n; }
sl_status_t sl_sleeptimer_start_timer_ms(sl_sleeptimer_timer_handle_t *h, uint32_t ms, sl_sleeptimer_timer_callback_t cb, void *d, uint8_t p, uint16_t f)
{
  (void)p; (void)f;
  if (!h) return 1;
  if (h->run) { vt_start_errors++; return SL_STATUS_NOT_READY; }
  if (ms > 131071000u) { vt_start_errors++; return SL_STATUS_INVALID_PARAMETER; }   // 2^32 ticks at 32768 Hz
  h->run = true; h->due_us = now_us + (uint64_t)ms * 1000; h->period_ms = 0; h->cb = cb; h->data = d; reg_add(h);
  return SL_STATUS_OK;
}
sl_status_t sl_sleeptimer_start_periodic_timer_ms(sl_sleeptimer_timer_handle_t *h, uint32_t ms, sl_sleeptimer_timer_callback_t cb, void *d, uint8_t p, uint16_t f)
{
  (void)p; (void)f;
  if (!h) return 1;
  if (h->run) { vt_start_errors++; return SL_STATUS_INVALID_STATE; }
  if (ms == 0 || ms > 131071000u) { vt_start_errors++; return SL_STATUS_INVALID_PARAMETER; }
  h->run = true; h->due_us = now_us + (uint64_t)ms * 1000; h->period_ms = ms; h->cb = cb; h->data = d; reg_add(h);
  return SL_STATUS_OK;
}
sl_status_t sl_sleeptimer_stop_timer(sl_sleeptimer_timer_handle_t *h) { if (!h) return 1; if (!h->run) return 0x0A; h->run = false; return SL_STATUS_OK; }
uint64_t sl_sleeptimer_get_tick_count64(void) { return now_us * SL_TICK_HZ / 1000000ULL; }
uint32_t sl_sleeptimer_get_tick_count(void) { return (uint32_t)sl_sleeptimer_get_tick_count64(); }
uint32_t sl_sleeptimer_tick_to_ms(uint32_t t) { return (uint32_t)(((uint64_t)t * 1000u) >> 15); }
sl_status_t sl_sleeptimer_tick64_to_ms(uint64_t t, uint64_t *ms) { *ms = (t * 1000u) >> 15; return SL_STATUS_OK; }
void vt_set_ms(uint64_t ms)
{
  uint64_t nu = ms * 1000; int64_t delta = (int64_t)nu - (int64_t)now_us;
  for (int i = 0; i < nreg; i++) if (reg[i]->run) reg[i]->due_us = (uint64_t)((int64_t)reg[i]->due_us + delta);
  now_us = nu;
}
void (*vt_hook)(void) = NULL;   // called after each fired timer (main-loop emulation)
void vt_advance_ms(uint64_t ms)
{
  uint64_t target = now_us + ms * 1000;
  for (;;) {
    sl_sleeptimer_timer_handle_t *best = NULL;
    for (int i = 0; i < nreg; i++) if (reg[i]->run && (!best || reg[i]->due_us < best->due_us)) best = reg[i];
    if (!best || best->due_us > target) { now_us = target; return; }
    if (best->due_us > now_us) now_us = best->due_us;
    if (best->period_ms) best->due_us += (uint64_t)best->period_ms * 1000; else best->run = false;
    core_depth_at_isr = core_depth;
    best->cb(best, best->data);
    if (core_depth != core_depth_at_isr) { fprintf(stderr, "ATOMIC imbalance in timer callback\n"); abort(); }
    if (vt_hook) vt_hook();
  }
}
long udelay_total_us = 0;
void sl_udelay_wait(unsigned us) { udelay_total_us += us; }

int core_depth = 0, core_max_depth = 0, core_depth_at_isr = 0;
int pm_em1 = 0, pm_em1_max = 0;
void sl_power_manager_add_em_requirement(sl_power_manager_em_t em) { if (em == SL_POWER_MANAGER_EM1) { pm_em1++; if (pm_em1 > pm_em1_max) pm_em1_max = pm_em1; } }
void sl_power_manager_remove_em_requirement(sl_power_manager_em_t em) { if (em == SL_POWER_MANAGER_EM1) { pm_em1--; if (pm_em1 < 0) { fprintf(stderr, "EM1 requirement removed below zero\n"); abort(); } } }
int cmu_timer1 = 0;
void CMU_ClockEnable(CMU_Clock_TypeDef c, bool en) { (void)c; cmu_timer1 = en; }

// ---------------- LED hardware model ----------------
GPIO_TypeDef gpio_regs;
TIMER_TypeDef timer1_regs;
int timer_enabled = 0; uint32_t timer_cmp[3];
uint8_t gpio_out[4][16];
int gpio_cs_low = 0;
USART_TypeDef fake_usart[2]; I2C_TypeDef fake_i2c[1];
hw_userdata_t fake_ud = { 0x1D, 0, 200, 200, 0, 0, 0x73 };      // EL016 by default; tests change it and call panel_init()
const hw_userdata_t *hw_userdata(void) { return &fake_ud; }
uint8_t hw_tag_type(void) { return fake_ud.tag; }
void TIMER_Init(TIMER_TypeDef *t, const TIMER_Init_TypeDef *i) { (void)t; (void)i; }
void TIMER_TopSet(TIMER_TypeDef *t, uint32_t v) { (void)t; (void)v; }
void TIMER_InitCC(TIMER_TypeDef *t, unsigned c, const TIMER_InitCC_TypeDef *i) { (void)t; (void)c; (void)i; }
void TIMER_CounterSet(TIMER_TypeDef *t, uint32_t v) { (void)t; (void)v; }
void TIMER_Enable(TIMER_TypeDef *t, bool en) { (void)t; timer_enabled = en; }
void TIMER_CompareSet(TIMER_TypeDef *t, unsigned c, uint32_t v) { (void)t; if (c < 3) timer_cmp[c] = v; }

// ---------------- NOR flash on SPI ----------------
uint8_t fl[524288];
uint32_t fl_erase_cnt[128];
int fl_violations = 0, fl_no_wel = 0, fl_prog_ops = 0, fl_erase_ops = 0, fl_read_when_down = 0;
bool fl_powered_down = false;
static bool wel = false;
int fl_cut_budget = -1;              // >=0: power fails when it reaches 0 (op torn, later ops ignored)
bool fl_dead = false;
int fl_busy_reads = 0, fl_stuck = 0; // busy for N status reads after an op; stuck = busy forever
static int busy_left = 0;
int spi_open_count = 0, spi_init_calls = 0, spi_deinit_calls = 0;
uint8_t fl_jedec[3] = { 0xA1, 0x28, 0x13 };
uint32_t fl_min_write = 0xFFFFFFFF;  // lowest address programmed or erased
uint32_t fl_below_slots_ops = 0;     // ops below 0x3C000
void fl_reset(void) { memset(fl, 0xFF, sizeof fl); memset(fl_erase_cnt, 0, sizeof fl_erase_cnt); fl_violations = fl_no_wel = fl_prog_ops = fl_erase_ops = 0; wel = false; fl_cut_budget = -1; fl_dead = false; fl_powered_down = false; fl_busy_reads = 0; fl_stuck = 0; busy_left = 0; fl_below_slots_ops = 0; fl_min_write = 0xFFFFFFFF; }

static uint8_t tx[300]; static int ntx = 0;
void GPIO_PinOutSet(GPIO_Port_TypeDef p, unsigned pin)
{
  if (p == gpioPortC && pin == 3) {
    if (gpio_cs_low && ntx > 0) {                     // end of a transaction
      uint8_t c = tx[0];
      if (c == 0x06) wel = true;
      else if (c == 0xB9) fl_powered_down = true;
      else if (c == 0xAB) fl_powered_down = false;
      else if (!fl_powered_down && c == 0x20 && ntx == 4) {
        uint32_t a = ((uint32_t)tx[1] << 16 | tx[2] << 8 | tx[3]) & ~4095u;
        if (!wel) fl_no_wel++;
        else if (!fl_dead) {
          if (fl_cut_budget == 0) { fl_dead = true; for (int i = 0; i < 4096; i += 2) fl[a + i] = 0xFF; for (int i = 1; i < 4096; i += 2) fl[a + i] &= (uint8_t)rand(); }
          else {
            if (fl_cut_budget > 0) fl_cut_budget--;
            memset(fl + a, 0xFF, 4096); fl_erase_cnt[a >> 12]++; fl_erase_ops++;
            if (a < fl_min_write) fl_min_write = a;
            if (a < 0x3C000) fl_below_slots_ops++;
            busy_left = fl_busy_reads;
          }
        }
        wel = false;
      } else if (!fl_powered_down && c == 0x02 && ntx >= 4) {
        uint32_t a = (uint32_t)tx[1] << 16 | tx[2] << 8 | tx[3];
        int n = ntx - 4;
        if (!wel) fl_no_wel++;
        else if (!fl_dead) {
          int torn = 0;
          if (fl_cut_budget == 0) { fl_dead = true; torn = 1; }
          else if (fl_cut_budget > 0) fl_cut_budget--;
          int m = torn ? n / 2 : n;
          for (int i = 0; i < m; i++) {
            uint32_t ad = (a & ~255u) | ((a + i) & 255u);       // page wrap
            if (((a + i) & ~255u) != (a & ~255u)) fl_violations++;
            if (tx[4 + i] & ~fl[ad]) fl_violations++;            // 0 -> 1 without erase
            fl[ad] &= tx[4 + i];
          }
          if (!torn) { fl_prog_ops++; if (a < fl_min_write) fl_min_write = a; if (a < 0x3C000) fl_below_slots_ops++; busy_left = fl_busy_reads; }
        }
        wel = false;
      }
    }
    gpio_cs_low = 0; ntx = 0;
  }
  if (p < 4 && pin < 16) gpio_out[p][pin] = 1;
}
void GPIO_PinOutClear(GPIO_Port_TypeDef p, unsigned pin) { if (p == gpioPortC && pin == 3) { gpio_cs_low = 1; ntx = 0; } if (p < 4 && pin < 16) gpio_out[p][pin] = 0; }
void GPIO_PinModeSet(GPIO_Port_TypeDef p, unsigned pin, GPIO_Mode_TypeDef m, unsigned out) { (void)p; (void)pin; (void)m; (void)out; }
Ecode_t SPIDRV_Init(SPIDRV_Handle_t h, SPIDRV_Init_t *i) { (void)i; if (h->open) { fprintf(stderr, "SPIDRV_Init on an open handle\n"); abort(); } h->open = 1; spi_open_count++; spi_init_calls++; return 0; }
Ecode_t SPIDRV_DeInit(SPIDRV_Handle_t h) { if (!h->open) { fprintf(stderr, "SPIDRV_DeInit on a closed handle\n"); abort(); } h->open = 0; spi_open_count--; spi_deinit_calls++; return 0; }
Ecode_t SPIDRV_MTransmitB(SPIDRV_Handle_t h, const void *b, int n)
{
  if (!h->open) { fprintf(stderr, "SPI use while closed\n"); abort(); }
  if (ntx + n > (int)sizeof tx) { fprintf(stderr, "tx overflow\n"); abort(); }
  memcpy(tx + ntx, b, (size_t)n); ntx += n; return 0;
}
static uint32_t rd_addr; static int rd_mode = 0;
Ecode_t SPIDRV_MReceiveB(SPIDRV_Handle_t h, void *b, int n)
{
  if (!h->open) { fprintf(stderr, "SPI use while closed\n"); abort(); }
  if (ntx == 4 && tx[0] == 0x03) {
    uint32_t a = (uint32_t)tx[1] << 16 | tx[2] << 8 | tx[3];
    if (fl_powered_down) fl_read_when_down++;
    if ((uint64_t)a + (uint64_t)n > sizeof fl) { fprintf(stderr, "read beyond flash\n"); abort(); }
    memcpy(b, fl + a, (size_t)n);
  } else memset(b, 0, (size_t)n);
  (void)rd_addr; (void)rd_mode;
  return 0;
}
Ecode_t SPIDRV_MTransferB(SPIDRV_Handle_t h, const void *txb, void *rxb, int n)
{
  if (!h->open) { fprintf(stderr, "SPI use while closed\n"); abort(); }
  uint8_t in[8]; if (n > 8) abort();
  memcpy(in, txb, (size_t)n);
  uint8_t out[8] = { 0 };
  if (in[0] == 0x05) { out[1] = (busy_left > 0 || fl_stuck) ? 1 : 0; if (busy_left > 0) busy_left--; }
  else if (in[0] == 0x9F) { if (!fl_powered_down) { out[1] = fl_jedec[0]; out[2] = fl_jedec[1]; out[3] = fl_jedec[2]; } }
  memcpy(rxb, out, (size_t)n);
  return 0;
}

// ---------------- bootloader ----------------
int btl_reboot_install = 0;
void bootloader_getInfo(BootloaderInformation_t *i) { i->version = 0x02040003; i->capabilities = 0x1234; }
void bootloader_rebootAndInstall(void) { btl_reboot_install++; }
