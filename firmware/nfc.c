// pin16: NFC chip TNB132M, see nfc.h. Own code.
#include <string.h>
#include "em_cmu.h"
#include "em_gpio.h"
#include "em_i2c.h"
#include "sl_udelay.h"
#include "hw.h"
#include "hwcard.h"
#include "nfc.h"

#define DEV_CTL   0x30            // control registers
#define DEV_WAKE  0x43
#define DEV_AI    0x48            // attribute information block
#define DEV_DATA  0x40            // NDEF blocks, sub-address 0x10 * (block number + 1)

static nfc_info_t info;
static volatile bool busy = false;

// ---- bus ----

static bool xfer(uint8_t dev7, uint16_t flags, uint8_t *w, uint16_t wl, uint8_t *r, uint16_t rl)
{
  I2C_TransferSeq_TypeDef t;
  t.addr = (uint16_t)(dev7 << 1);
  t.flags = flags;
  t.buf[0].data = w;  t.buf[0].len = wl;
  t.buf[1].data = r;  t.buf[1].len = rl;
  I2C_TransferReturn_TypeDef res = I2C_TransferInit(I2C0, &t);
  uint32_t guard = 400000;                           // never wait forever for a stuck bus
  while (res == i2cTransferInProgress && --guard) {
    res = I2C_Transfer(I2C0);
  }
  if (res == i2cTransferInProgress) {
    I2C0->CMD = I2C_CMD_ABORT;
    HWC_I2C(dev7, flags, 0, w, wl, r, 0);
    return false;
  }
  HWC_I2C(dev7, flags, res == i2cTransferDone, w, wl, r, (res == i2cTransferDone && (flags & I2C_FLAG_WRITE_READ)) ? rl : 0);
  return res == i2cTransferDone;
}

static bool rd_block(uint8_t dev, uint8_t sub, uint8_t *out16)
{
  return xfer(dev, I2C_FLAG_WRITE_READ, &sub, 1, out16, 16);
}

static bool wr_block(uint8_t dev, uint8_t sub, const uint8_t *in16)
{
  return xfer(dev, I2C_FLAG_WRITE_WRITE, &sub, 1, (uint8_t *)in16, 16);
}

static void open_io(void)
{
  const hw_nfc_t *n = &hw_pins.nfc;
  GPIO_PinModeSet((GPIO_Port_TypeDef)n->scl.port, n->scl.pin, gpioModeWiredAndFilter, 0);
  GPIO_PinModeSet((GPIO_Port_TypeDef)n->sda.port, n->sda.pin, gpioModeWiredAndFilter, 0);
  GPIO_PinModeSet((GPIO_Port_TypeDef)n->pwr.port, n->pwr.pin, gpioModePushPull, 1);
  sl_udelay_wait(40000);                             // the chip needs about 40 ms after its supply is on

  CMU_ClockEnable(cmuClock_I2C0, true);
  GPIO->I2CROUTE[0].SDAROUTE = (GPIO->I2CROUTE[0].SDAROUTE & ~_GPIO_I2C_SDAROUTE_MASK)
                             | ((uint32_t)n->sda.port << _GPIO_I2C_SDAROUTE_PORT_SHIFT)
                             | ((uint32_t)n->sda.pin << _GPIO_I2C_SDAROUTE_PIN_SHIFT);
  GPIO->I2CROUTE[0].SCLROUTE = (GPIO->I2CROUTE[0].SCLROUTE & ~_GPIO_I2C_SCLROUTE_MASK)
                             | ((uint32_t)n->scl.port << _GPIO_I2C_SCLROUTE_PORT_SHIFT)
                             | ((uint32_t)n->scl.pin << _GPIO_I2C_SCLROUTE_PIN_SHIFT);
  GPIO->I2CROUTE[0].ROUTEEN = GPIO_I2C_ROUTEEN_SDAPEN | GPIO_I2C_ROUTEEN_SCLPEN;

  I2C_Init_TypeDef init = I2C_INIT_DEFAULT;          // standard mode, master
  I2C_Init(I2C0, &init);
  I2C0->CTRL = I2C_CTRL_AUTOSN;                      // stop by itself after a NACK
}

static void close_io(void)
{
  const hw_nfc_t *n = &hw_pins.nfc;
  I2C_Reset(I2C0);
  CMU_ClockEnable(cmuClock_I2C0, false);
  GPIO->I2CROUTE[0].ROUTEEN = 0;
  GPIO_PinOutClear((GPIO_Port_TypeDef)n->pwr.port, n->pwr.pin);
  GPIO_PinModeSet((GPIO_Port_TypeDef)n->pwr.port, n->pwr.pin, gpioModeInput, 1);
  GPIO_PinModeSet((GPIO_Port_TypeDef)n->scl.port, n->scl.pin, gpioModeInput, 1);
  GPIO_PinModeSet((GPIO_Port_TypeDef)n->sda.port, n->sda.pin, gpioModeInput, 1);
}

// ---- exchanges ----

// The wake-up exchange of the stock firmware. Its answers are only recorded: a failed step does not stop the rest.
static void wake(void)
{
  uint8_t w[2] = { 0x21, 0x04 };
  uint8_t sub = 0x25, v = 0, b[16];
  (void)xfer(DEV_CTL, I2C_FLAG_WRITE, w, 2, NULL, 0);
  if (xfer(DEV_CTL, I2C_FLAG_WRITE_READ, &sub, 1, &v, 1)) {
    info.reg25 = v;
  }
  sl_udelay_wait(20000);
  (void)rd_block(DEV_WAKE, 0x30, b);
  sl_udelay_wait(2000);
}

// One NDEF block, the way the stock firmware reads it: attribute block first, then the data block.
static bool rd_data(uint8_t blk, uint8_t *out16)
{
  uint8_t junk[16];
  (void)rd_block(DEV_AI, 0x00, junk);
  sl_udelay_wait(500);
  bool ok = rd_block(DEV_DATA, (uint8_t)(0x10 + 0x10 * blk), out16);
  sl_udelay_wait(200);
  return ok;
}

static uint16_t nmaxb_of(const uint8_t *ai)
{
  uint16_t n = (uint16_t)((ai[3] << 8) | ai[4]);
  return (ai[0] == 0x10 && n >= 1 && n <= 0x100) ? n : 0;
}

static void make_ai(uint8_t *ai, uint32_t ln, uint8_t rwflag, uint16_t nmaxb)
{
  memset(ai, 0, 16);
  ai[0] = 0x10; ai[1] = 0x02; ai[2] = 0x01;                                   // version 1.0, Nbr 2, Nbw 1
  ai[3] = (uint8_t)(nmaxb >> 8); ai[4] = (uint8_t)nmaxb;                      // Nmaxb: what the chip says (60 if it says nothing)
  ai[10] = rwflag;
  ai[11] = (uint8_t)(ln >> 16); ai[12] = (uint8_t)(ln >> 8); ai[13] = (uint8_t)ln;
  uint16_t s = 0;
  for (int i = 0; i < 14; i++) s = (uint16_t)(s + ai[i]);
  ai[14] = (uint8_t)(s >> 8); ai[15] = (uint8_t)s;
}

// Reads the first record into info. The chip is open.
static void read_record(void)
{
  uint8_t d[NDEF_MAX + 16];
  info.text[0] = 0;
  info.kind = NDEF_NONE;
  uint32_t ln = ((uint32_t)info.ai[11] << 16) | ((uint32_t)info.ai[12] << 8) | info.ai[13];
  info.ndef_len = (uint16_t)(ln > 0xFFFF ? 0xFFFF : ln);
  if (ln < 3) {
    return;
  }
  if (ln > NDEF_MAX) {
    info.kind = NDEF_OTHER;                          // longer than what is shown (a factory message, say): the first record is not parsed
    return;
  }
  for (unsigned b = 0; b < (ln + 15) / 16; b++) {
    if (!rd_data((uint8_t)b, &d[b * 16])) {
      return;
    }
  }
  info.kind = (uint8_t)ndef_parse(d, ln, info.text, sizeof(info.text));
}

bool nfc_probe(void)
{
  busy = true;
  memset(&info, 0, sizeof(info));
  open_io();
  wake();
  if (rd_block(DEV_AI, 0x00, info.ai) && info.ai[0] == 0x10) {
    info.present = true;
    info.cap = (uint16_t)(nmaxb_of(info.ai) * 16);
    read_record();
  } else if (info.ai[0] != 0) {
    info.present = true;                             // the chip answers but holds no record (blank)
  }
  close_io();
  busy = false;
  return info.present;
}

const nfc_info_t *nfc_info(void) { return &info; }
bool nfc_busy(void) { return busy; }

nfc_status_t nfc_write(ndef_kind_t kind, const char *a, const char *b, uint8_t auth)
{
  uint8_t rec[NDEF_MAX + 16];
  size_t n = 0;
  memset(rec, 0, sizeof(rec));
  switch (kind) {
    case NDEF_URI:  n = ndef_uri(rec, a); break;
    case NDEF_TEXT: n = ndef_text(rec, a); break;
    case NDEF_WIFI: n = ndef_wifi(rec, a, b, auth); break;
    default: break;
  }
  if (n == 0) {
    ndef_wipe(rec, sizeof(rec));
    return NFC_E_ARG;
  }
  if (busy) {
    ndef_wipe(rec, sizeof(rec));
    return NFC_E_BUSY;
  }
  uint32_t ln = (uint32_t)n;
  unsigned blocks = (ln + 15) / 16;

  busy = true;
  nfc_status_t st = NFC_E_BUS;
  uint8_t cur[16], ai[16], back[16];
  open_io();
  wake();
  uint8_t rw = 0;
  uint16_t nmax = 0x3C;
  if (rd_block(DEV_AI, 0x00, cur) && (cur[0] & 0xF0) == 0x10) {
    rw = cur[10];                                    // keep the flag the chip has
    if (nmaxb_of(cur)) nmax = nmaxb_of(cur);         // and its size
  }
  if (ln > (uint32_t)nmax * 16) {
    st = NFC_E_ARG;                                  // the message does not fit this chip
    goto done;
  }
  make_ai(ai, ln, rw, nmax);
  if (!wr_block(DEV_AI, 0x00, ai)) {
    goto done;
  }
  sl_udelay_wait(10000);
  for (unsigned k = 0; k < blocks; k++) {
    if (!wr_block(DEV_DATA, (uint8_t)(0x10 + 0x10 * k), &rec[k * 16])) {
      goto done;
    }
    sl_udelay_wait(10000);
  }
  // read it back
  st = NFC_E_VERIFY;
  if (!rd_block(DEV_AI, 0x00, back) || memcmp(back, ai, 16) != 0) {
    goto done;
  }
  for (unsigned k = 0; k < blocks; k++) {
    if (!rd_data((uint8_t)k, back) || memcmp(back, &rec[k * 16], 16) != 0) {
      goto done;
    }
  }
  memcpy(info.ai, ai, 16);
  info.cap = (uint16_t)(nmax * 16);
  info.ndef_len = (uint16_t)ln;
  info.kind = (uint8_t)ndef_parse(rec, ln, info.text, sizeof(info.text));
  st = NFC_OK;
done:
  close_io();
  busy = false;
  ndef_wipe(rec, sizeof(rec));                       // the record may hold a Wi-Fi key
  return st;
}

nfc_status_t nfc_write_url(const char *url) { return nfc_write(NDEF_URI, url, NULL, 0); }
