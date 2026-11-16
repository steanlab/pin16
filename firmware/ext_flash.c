// pin16: access to the external SPI flash. Own code.
#include <spidrv.h>
#include "em_gpio.h"
#include "sl_udelay.h"
#include "sl_sleeptimer.h"
#include "hw.h"
#include "ext_flash.h"

static const hw_flash_t *fc = &hw_pins.flash;
static SPIDRV_HandleData_t hd;
static SPIDRV_Handle_t h = &hd;
static bool is_open = false;

#define CS_LOW()   GPIO_PinOutClear((GPIO_Port_TypeDef)fc->cs.port, fc->cs.pin)
#define CS_HIGH()  GPIO_PinOutSet((GPIO_Port_TypeDef)fc->cs.port, fc->cs.pin)

static void cmd1(uint8_t c)
{
  CS_LOW();
  SPIDRV_MTransmitB(h, &c, 1);
  CS_HIGH();
}

static uint8_t rdsr(void)
{
  uint8_t b[2] = { 0x05, 0 };
  CS_LOW();
  SPIDRV_MTransferB(h, b, b, 2);
  CS_HIGH();
  return b[1];
}

static bool wait_ready(uint32_t timeout_ms)
{
  uint32_t loops = timeout_ms * 10UL;           // 100 us per loop
  while (loops--) {
    if ((rdsr() & 0x01) == 0) {
      return true;
    }
    sl_udelay_wait(100);
  }
  return false;
}

static bool ota_mode = false;

void xf_ota_mode(bool on) { ota_mode = on; }

static bool write_ok_area(uint32_t addr, uint32_t len)
{
  if (ota_mode) {
    return len <= XF_OTA_END && addr <= XF_OTA_END - len;     // only the OTA slot
  }
  return addr >= XF_WRITE_MIN && len <= XF_SIZE && addr <= XF_SIZE - len;
}

bool xf_begin(uint8_t jedec[3])
{
  if (is_open) {
    return false;
  }
  GPIO_PinModeSet((GPIO_Port_TypeDef)fc->cs.port, fc->cs.pin, gpioModePushPull, 1);

  SPIDRV_Init_t si = SPIDRV_MASTER_DEFAULT;
  si.port = fc->usart;
  si.portTx = (GPIO_Port_TypeDef)fc->mosi.port;
  si.pinTx = fc->mosi.pin;
  si.portRx = (GPIO_Port_TypeDef)fc->miso.port;
  si.pinRx = fc->miso.pin;
  si.portClk = (GPIO_Port_TypeDef)fc->sck.port;
  si.pinClk = fc->sck.pin;
  si.bitRate = 8000000;
  si.csControl = spidrvCsControlApplication;
  if (SPIDRV_Init(h, &si) != ECODE_EMDRV_SPIDRV_OK) {
    return false;
  }
  is_open = true;

  // Wake up from deep power down: CS pulse (MX25 style), then the standard release command
  CS_LOW();
  sl_udelay_wait(20);
  CS_HIGH();
  sl_udelay_wait(35);
  cmd1(0xAB);
  sl_udelay_wait(100);

  bool ok = false;
  for (int attempt = 0; attempt < 3 && !ok; attempt++) {
    uint8_t b[4] = { 0x9F, 0, 0, 0 };
    CS_LOW();
    SPIDRV_MTransferB(h, b, b, 4);
    CS_HIGH();
    jedec[0] = b[1];
    jedec[1] = b[2];
    jedec[2] = b[3];
    ok = (b[1] != 0x00 && b[1] != 0xFF);
    if (!ok) {
      sl_udelay_wait(1000);
    }
  }
  if (!ok) {
    xf_end();
    return false;
  }
  wait_ready(50);
  return true;
}

// Reads the SFDP table (command 5A: 3 address bytes, 1 dummy byte). Diagnostic use: the flash must be open.
bool xf_sfdp(uint32_t addr, uint8_t *buf, uint32_t len)
{
  if (!is_open || len > 2048) {
    return false;
  }
  uint8_t rc[5] = { 0x5A, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr, 0 };
  CS_LOW();
  SPIDRV_MTransmitB(h, rc, 5);
  SPIDRV_MReceiveB(h, buf, (int)len);
  CS_HIGH();
  return true;
}

void xf_end(void)
{
  if (!is_open) {
    return;
  }
  cmd1(0xB9);                                   // deep power down
  sl_udelay_wait(30);
  GPIO_PinModeSet((GPIO_Port_TypeDef)fc->cs.port, fc->cs.pin, gpioModeInputPull, 1);
  SPIDRV_DeInit(h);
  is_open = false;
}

void xf_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
  if (!is_open || addr >= XF_SIZE) {
    return;
  }
  if (len > XF_SIZE - addr) {
    len = XF_SIZE - addr;
  }
  while (len) {
    uint32_t n = len > 2048 ? 2048 : len;       // the DMA does at most 2 KB at once
    uint8_t rc[4] = { 0x03, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr };
    CS_LOW();
    SPIDRV_MTransmitB(h, rc, 4);
    SPIDRV_MReceiveB(h, buf, (int)n);
    CS_HIGH();
    addr += n;
    buf += n;
    len -= n;
  }
}

bool xf_erase_sector(uint32_t addr)
{
  if (!is_open || !write_ok_area(addr, XF_SECTOR)) {
    return false;
  }
  addr &= ~(XF_SECTOR - 1);
  cmd1(0x06);                                   // write enable
  uint8_t ec[4] = { 0x20, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr };
  CS_LOW();
  SPIDRV_MTransmitB(h, ec, 4);
  CS_HIGH();
  return wait_ready(500);                       // sector erase: up to 400 ms
}

bool xf_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
  if (!is_open || !write_ok_area(addr, len)) {
    return false;
  }
  while (len) {
    uint32_t n = XF_PAGE - (addr & (XF_PAGE - 1));
    if (n > len) {
      n = len;
    }
    cmd1(0x06);                                 // write enable
    uint8_t pc[4] = { 0x02, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr };
    CS_LOW();
    SPIDRV_MTransmitB(h, pc, 4);
    SPIDRV_MTransmitB(h, data, (int)n);
    CS_HIGH();
    if (!wait_ready(20)) {                      // page program: up to 3 ms
      return false;
    }
    addr += n;
    data += n;
    len -= n;
  }
  return true;
}

static uint32_t ms_now(void)
{
  return sl_sleeptimer_tick_to_ms(sl_sleeptimer_get_tick_count());
}

uint8_t xf_selftest(uint16_t *erase_ms, uint16_t *prog_ms, uint16_t *bad_index)
{
  uint8_t id[3];
  uint8_t pat[256] = { 0 }, rd[256] = { 0 };
  uint8_t res = 0;
  *erase_ms = 0;
  *prog_ms = 0;
  *bad_index = 0;
  if (!xf_begin(id)) {
    return 1;
  }
  uint32_t t0 = ms_now();
  if (!xf_erase_sector(XF_TEST_SECTOR)) {
    res = 2;
    goto done;
  }
  *erase_ms = (uint16_t)(ms_now() - t0);

  xf_read(XF_TEST_SECTOR, rd, sizeof(rd));
  for (uint32_t i = 0; i < sizeof(rd); i++) {
    if (rd[i] != 0xFF) {
      *bad_index = (uint16_t)i;
      res = 3;
      goto done;
    }
  }
  for (uint32_t i = 0; i < sizeof(pat); i++) {
    pat[i] = (uint8_t)(i ^ 0xA5);
  }
  t0 = ms_now();
  if (!xf_write(XF_TEST_SECTOR, pat, sizeof(pat))) {
    res = 4;
    goto done;
  }
  *prog_ms = (uint16_t)(ms_now() - t0);

  xf_read(XF_TEST_SECTOR, rd, sizeof(rd));
  for (uint32_t i = 0; i < sizeof(rd); i++) {
    if (rd[i] != pat[i]) {
      *bad_index = (uint16_t)i;
      res = 5;
      goto done;
    }
  }
  if (!xf_erase_sector(XF_TEST_SECTOR)) {
    res = 2;
  }
done:
  xf_end();
  return res;
}
