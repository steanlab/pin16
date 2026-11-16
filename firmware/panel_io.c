// pin16: lowest layer of the panel, see panel_io.h. Own code.
#include <stddef.h>
#include "em_gpio.h"
#include "em_usart.h"
#include "gpiointerrupt.h"
#include "spidrv.h"
#include "sl_sleeptimer.h"
#include "sl_power_manager.h"
#include "sl_udelay.h"
#include "hw.h"
#include "hwcard.h"
#include "panel_io.h"

#define PN  (hw_pins.panel)
#define PORT(p)  ((GPIO_Port_TypeDef)(p).port)
#define TFLAGS   SL_SLEEPTIMER_NO_HIGH_PRECISION_HF_CLOCKS_REQUIRED_FLAG

static SPIDRV_HandleData_t spi_data;
static SPIDRV_Handle_t     spi = &spi_data;
static sl_sleeptimer_timer_handle_t tmr;
static volatile bool tmr_fired = false;
static volatile uint8_t busy_want = 1;

static void cs_low(void)  { GPIO_PinOutClear(PORT(PN.cs), PN.cs.pin); }
static void cs_high(void) { GPIO_PinOutSet(PORT(PN.cs), PN.cs.pin); }
static void dc_cmd(void)  { GPIO_PinOutClear(PORT(PN.dc), PN.dc.pin); }
static void dc_data(void) { GPIO_PinOutSet(PORT(PN.dc), PN.dc.pin); }

static void off_pin(const hw_pin_t *p) { GPIO_PinModeSet((GPIO_Port_TypeDef)p->port, p->pin, gpioModeInputPull, 0); }

void pio_idle(void)
{
  GPIO_PinModeSet(PORT(PN.en), PN.en.pin, gpioModePushPull, 0);              // supply off
  off_pin(&PN.mosi); off_pin(&PN.sck); off_pin(&PN.cs); off_pin(&PN.busy); off_pin(&PN.dc); off_pin(&PN.rst);
}

static void on_busy(uint8_t pin, void *ctx)
{
  (void)ctx; (void)pin;
  if (GPIO_PinInGet(PORT(PN.busy), PN.busy.pin) == busy_want) {
    GPIO_IntDisable(1u << PN.busy.pin);
    GPIO_IntClear(1u << PN.busy.pin);
  }
}

void pio_power_on(void)
{
  GPIO_PinModeSet(PORT(PN.en), PN.en.pin, gpioModePushPull, 1);              // supply on
  GPIO_PinModeSet(PORT(PN.cs), PN.cs.pin, gpioModePushPull, 1);              // chip select by hand
  GPIO_PinModeSet(PORT(PN.dc), PN.dc.pin, gpioModePushPull, 1);
  GPIO_PinModeSet(PORT(PN.rst), PN.rst.pin, gpioModePushPull, 1);
  GPIO_PinModeSet(PORT(PN.busy), PN.busy.pin, gpioModeInput, 1);
  unsigned int irq = GPIOINT_CallbackRegisterExt(PN.busy.pin, on_busy, NULL);
  GPIO_ExtIntConfig(PORT(PN.busy), PN.busy.pin, irq, true, true, false);     // both edges, enabled only while waiting

  SPIDRV_Init_t si = SPIDRV_MASTER_DEFAULT;                                   // mode 0, MSB first
  si.port = PN.usart;
  si.portTx = PORT(PN.mosi);  si.pinTx = PN.mosi.pin;
  si.portRx = PORT(PN.mosi);  si.pinRx = PN.mosi.pin;                         // no MISO line: the driver needs a pin, MOSI serves
  si.portClk = PORT(PN.sck);  si.pinClk = PN.sck.pin;
  si.bitRate = 5000000;
  si.csControl = spidrvCsControlApplication;
  SPIDRV_Init(spi, &si);
  GPIO_PinModeSet(PORT(PN.mosi), PN.mosi.pin, gpioModePushPull, 0);
  HWC_EVENT('A');
}

void pio_release(void)
{
  HWC_EVENT('Z');                                                             // levels as the panel last saw them
  off_pin(&PN.mosi); off_pin(&PN.sck); off_pin(&PN.cs); off_pin(&PN.busy); off_pin(&PN.dc); off_pin(&PN.rst);
  GPIO_PinModeSet(PORT(PN.en), PN.en.pin, gpioModeInputPull, 0);              // supply off, pulled down
  SPIDRV_DeInit(spi);
}

static void on_timer(sl_sleeptimer_timer_handle_t *h, void *d)
{
  (void)h; (void)d;
  tmr_fired = true;
}

void pio_wait_ms(uint32_t ms)
{
  HWC_WAIT(ms);
  tmr_fired = false;
  if (sl_sleeptimer_restart_timer_ms(&tmr, ms, on_timer, NULL, 1, TFLAGS) == SL_STATUS_OK) {
    while (!tmr_fired) {
      sl_power_manager_sleep();
    }
  } else {                                                                    // no timer: wait with the CPU
    while (ms > 100) {
      sl_udelay_wait(100000);
      ms -= 100;
    }
    sl_udelay_wait(ms * 1000);
  }
}

void pio_rst(bool high)
{
  if (high) {
    GPIO_PinOutSet(PORT(PN.rst), PN.rst.pin);
  } else {
    GPIO_PinOutClear(PORT(PN.rst), PN.rst.pin);
  }
  HWC_EVENT('N');
}

void pio_cmd(uint8_t op, const uint8_t *data, uint8_t n, bool keep_cs)
{
  HWC_CMD(op, data, n);
  dc_cmd();
  cs_low();
  SPIDRV_MTransmitB(spi, &op, 1);
  dc_data();
  if (n > 0) {
    cs_high();                                                                // the data follow after a chip-select pulse
    sl_udelay_wait(1);
    cs_low();
    SPIDRV_MTransmitB(spi, data, n);
  }
  if (!keep_cs) {
    cs_high();
  }
}

void pio_data(const uint8_t *data, size_t n)
{
  HWC_DATA(data, n);
  dc_data();
  cs_low();
  SPIDRV_MTransmitB(spi, data, n);
}

void pio_read(uint8_t *data, size_t n)
{
  PN.usart->CTRL_SET = USART_CTRL_LOOPBK_ENABLE;                              // the panel drives the data line itself
  PN.usart->CMD = USART_CMD_TXTRIEN;
  dc_data();
  cs_low();
  SPIDRV_MTransferB(spi, data, data, n);
  PN.usart->CTRL_CLR = USART_CTRL_LOOPBK_ENABLE;
  PN.usart->CMD = USART_CMD_TXTRIDIS;
  HWC_READ(data, n);
}

uint32_t pio_wait_busy(uint32_t timeout_ms, uint8_t level)
{
  HWC_BUSY_BEGIN(level, timeout_ms);
  uint32_t t0 = sl_sleeptimer_get_tick_count();
  busy_want = level;
  GPIO_IntClear(1u << PN.busy.pin);
  GPIO_IntEnable(1u << PN.busy.pin);
  while (GPIO_PinInGet(PORT(PN.busy), PN.busy.pin) != level) {
    // the edge interrupt wakes the chip; the 200 ms timer only guards against an edge that came between the check and the sleep
    sl_sleeptimer_restart_timer_ms(&tmr, 200, on_timer, NULL, 0, TFLAGS);
    sl_power_manager_sleep();
  }
  sl_sleeptimer_stop_timer(&tmr);
  GPIO_IntDisable(1u << PN.busy.pin);
  GPIO_IntClear(1u << PN.busy.pin);
  uint32_t ms = sl_sleeptimer_tick_to_ms(sl_sleeptimer_get_tick_count() - t0);
  HWC_BUSY_END(ms);
  return ms;
}
