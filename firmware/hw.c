// pin16: the tag hardware, see hw.h. Own code.
#include <stddef.h>
#include "em_cmu.h"
#include "em_emu.h"
#include "em_gpio.h"
#include "em_rmu.h"
#include "em_wdog.h"
#include "gpiointerrupt.h"
#include "sl_sleeptimer.h"
#include "sl_power_manager.h"
#include "sl_iostream.h"
#include "sl_iostream_handles.h"
#include "hw.h"
#include "nfc.h"

#define P(p, n) { (uint8_t)(p), (n) }

const hw_tag_t hw_pins = {
  .flash = { .usart = USART0, .mosi = P(gpioPortC, 1), .miso = P(gpioPortC, 0), .sck = P(gpioPortC, 2), .cs = P(gpioPortC, 3) },
  .panel = { .usart = USART1, .mosi = P(gpioPortA, 3), .sck = P(gpioPortA, 4), .cs = P(gpioPortB, 0), .dc = P(gpioPortA, 6),
             .rst = P(gpioPortA, 7), .busy = P(gpioPortA, 8), .en = P(gpioPortA, 0) },
  .nfc   = { .i2c = I2C0, .sda = P(gpioPortD, 3), .scl = P(gpioPortD, 1), .pwr = P(gpioPortD, 0), .fd = P(gpioPortD, 2) },
  .btn   = { P(gpioPortB, 1), P(gpioPortA, 5) },
  .led   = { P(gpioPortC, 5), P(gpioPortC, 6), P(gpioPortC, 7) },       // red, green, blue (found by looking at the tag)
  .dbg_tx = P(gpioPortB, 2), .dbg_rx = P(gpioPortB, 3),
};

// ---- factory data ----

const hw_userdata_t *hw_userdata(void)
{
  static hw_userdata_t u;
  static bool have = false;
  if (!have) {
    const uint8_t *b = (const uint8_t *)USERDATA_BASE;
    u.ctrl  = b[0x09];
    u.color = b[0x0A];
    u.xres  = (uint16_t)(b[0x0B] | (b[0x0C] << 8));
    u.yres  = (uint16_t)(b[0x0D] | (b[0x0E] << 8));
    u.capa0 = b[0x12];
    u.capa1 = b[0x13];
    u.tag   = b[0x16];
    have = true;
  }
  return &u;
}

uint8_t hw_tag_type(void) { return hw_userdata()->tag; }

int8_t hw_temperature(void) { return (int8_t)EMU_TemperatureGet(); }

// ---- inputs ----

static hw_input_cb_t in_cb = NULL;
static sl_sleeptimer_timer_handle_t nfc_poll_timer;

static GPIO_Port_TypeDef prt(const hw_pin_t *p) { return (GPIO_Port_TypeDef)p->port; }

static void on_edge(uint8_t pin, void *ctx)
{
  (void)ctx;
  if (in_cb == NULL) {
    return;
  }
  if (pin == hw_pins.btn[0].pin) {
    in_cb(HW_IN_BTN1);
  } else if (pin == hw_pins.btn[1].pin) {
    in_cb(HW_IN_BTN2);
  } else if (pin == hw_pins.nfc.fd.pin && !nfc_busy()) {
    in_cb(HW_IN_NFC);
  }
}

// Ports C and D cannot wake the chip with an interrupt from EM2: while it sleeps, the field detect pin (and, as the stock
// firmware did, the supply pin of the NFC chip) is looked at every 200 ms.
static void nfc_poll_cb(sl_sleeptimer_timer_handle_t *h, void *d)
{
  (void)h; (void)d;
  const hw_nfc_t *n = &hw_pins.nfc;
  if (nfc_busy()) {
    return;
  }
  if (GPIO_PinInGet(prt(&n->fd), n->fd.pin) || GPIO_PinInGet(prt(&n->pwr), n->pwr.pin)) {
    on_edge(n->fd.pin, NULL);
  }
}

static void on_em(sl_power_manager_em_t from, sl_power_manager_em_t to)
{
  (void)from;
  if (to == SL_POWER_MANAGER_EM2 || to == SL_POWER_MANAGER_EM3) {
    sl_sleeptimer_start_periodic_timer_ms(&nfc_poll_timer, 200, nfc_poll_cb, NULL, 0xFF, SL_SLEEPTIMER_NO_HIGH_PRECISION_HF_CLOCKS_REQUIRED_FLAG);
  } else {
    sl_sleeptimer_stop_timer(&nfc_poll_timer);
  }
}

static sl_power_manager_em_transition_event_handle_t em_handle;
static const sl_power_manager_em_transition_event_info_t em_info = {
  .event_mask = SL_POWER_MANAGER_EVENT_TRANSITION_ENTERING_EM2 | SL_POWER_MANAGER_EVENT_TRANSITION_ENTERING_EM3
              | SL_POWER_MANAGER_EVENT_TRANSITION_LEAVING_EM2 | SL_POWER_MANAGER_EVENT_TRANSITION_LEAVING_EM3,
  .on_event = on_em
};

static void watch(const hw_pin_t *p, bool both_edges)
{
  unsigned int irq = GPIOINT_CallbackRegisterExt(p->pin, on_edge, NULL);
  GPIO_ExtIntConfig(prt(p), p->pin, irq, true, both_edges, true);
  GPIO_IntClear(1u << p->pin);
  GPIO_IntEnable(1u << p->pin);
}

void hw_inputs_start(hw_input_cb_t cb)
{
  in_cb = cb;
  watch(&hw_pins.btn[0], true);                     // the buttons pull the pin low: both edges are watched, the app ignores repeats
  watch(&hw_pins.btn[1], true);
  watch(&hw_pins.nfc.fd, false);                    // rising edge only
}

// ---- start-up ----

void hw_init(void)
{
  // Decoy pins that the generated debug UART configuration uses: not connected on the tag
  for (uint8_t p = 5; p <= 8; p++) {
    GPIO_PinModeSet(gpioPortB, p, gpioModeDisabled, 1);
  }
  // The SWO pin is the panel MOSI line on this tag: keep the debug unit away from it
  GPIO_DbgSWOEnable(false);
  GPIO_PinModeSet((GPIO_Port_TypeDef)GPIO_SWV_PORT, GPIO_SWV_PIN, gpioModeDisabled, 0);

  // Debug UART on its pins
  extern sl_iostream_instance_info_t sl_iostream_instance_euart_debug_info;
  const hw_pin_t *tx = &hw_pins.dbg_tx, *rx = &hw_pins.dbg_rx;
  GPIO_PinModeSet(prt(tx), tx->pin, gpioModePushPull, 1);
  GPIO->EUARTROUTE->TXROUTE = ((uint32_t)tx->port << _GPIO_EUART_TXROUTE_PORT_SHIFT) | ((uint32_t)tx->pin << _GPIO_EUART_TXROUTE_PIN_SHIFT);
  GPIO_PinModeSet(prt(rx), rx->pin, gpioModeInput, 0);
  GPIO->EUARTROUTE->RXROUTE = ((uint32_t)rx->port << _GPIO_EUART_RXROUTE_PORT_SHIFT) | ((uint32_t)rx->pin << _GPIO_EUART_RXROUTE_PIN_SHIFT);
  sl_iostream_set_system_default(sl_iostream_instance_euart_debug_info.handle);

  RMU_ResetCauseClear();                            // sysinfo_early() has taken it already

  // Buttons: pulled up, field detect: plain input, LED pins high (off)
  for (int i = 0; i < 2; i++) {
    GPIO_PinModeSet(prt(&hw_pins.btn[i]), hw_pins.btn[i].pin, gpioModeInputPullFilter, 1);
  }
  GPIO_PinModeSet(prt(&hw_pins.nfc.fd), hw_pins.nfc.fd.pin, gpioModeInput, 0);
  for (int i = 0; i < 3; i++) {
    GPIO_PinModeSet(prt(&hw_pins.led[i]), hw_pins.led[i].pin, gpioModePushPull, 1);
  }

  (void)nfc_probe();                                // wake-up exchange and the URL in the chip, the chip is powered off after it

  sl_power_manager_subscribe_em_transition_event(&em_handle, &em_info);
}

// ---- faults: reset through the watchdog after a second ----

static void fault(void)
{
  WDOG_Init_TypeDef w = WDOG_INIT_DEFAULT;
  w.perSel = wdogPeriod_1k;
  WDOGn_Init(WDOG0, &w);
  while (1) {
  }
}

void NMI_Handler(void)       { fault(); }
void HardFault_Handler(void) { fault(); }
void BusFault_Handler(void)  { fault(); }
