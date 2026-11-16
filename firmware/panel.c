// pin16: the e-paper panel, see panel.h. Own code.
// Every sequence below was recorded from real tags (docs/hardware.md) and is replayed byte for byte by the PC test (tests/host/test_panel.c).
#include <string.h>
#include "hw.h"
#include "panel_io.h"
#include "panel.h"

#define CMD(op, ...) do { static const uint8_t d_[] = { __VA_ARGS__ }; pio_cmd((op), d_, (uint8_t)sizeof(d_), false); } while (0)
#define CMD0(op)     pio_cmd((op), NULL, 0, false)

typedef struct {
  const char *name;
  uint16_t w, h;                    // pixels per scan line, scan lines
  bool mirror;                      // slide line 0 is the last scan line
  bool flip_x;                      // slide pixel 0 is the last pixel of a scan line
  uint8_t resets;                   // reset pulses before the set-up commands
  void (*setup)(void);
} profile_t;

static const profile_t *prof = NULL;
static uint8_t line[PANEL_MAX_LINE];

// ---- set-up commands ----

static void setup_el016(void)
{
  CMD(0x00, 0x0F, 0x09);
  CMD(0x01, 0x07);
  CMD(0x06, 0x40, 0x00, 0x00);
  CMD(0x61, 0x00, 0xC8, 0x00, 0xC8);                  // 200 x 200
  CMD(0x50, 0x37);
  CMD(0x30, 0x08);
  CMD(0xE7, 0x1C);
  CMD(0xE9, 0x01);
  CMD(0xFF, 0xA5);
  CMD(0xEF, 0x03, 0x2C, 0x07, 0x20, 0x07, 0x10, 0x09, 0x21);
  CMD(0xDC, 0x01);
  CMD(0xDD, 0x06);
  CMD(0xDE, 0x46);
  CMD(0xDA, 0x08);
  CMD(0xE8, 0x00);
  CMD(0xFF, 0xE3);
}

static void setup_el022(void)
{
  CMD(0x4D, 0x78);
  CMD(0x00, 0x07, 0x09);
  CMD(0x01, 0x07);
  CMD(0x03, 0x10, 0x54, 0x44);
  CMD(0x06, 0x0F, 0x0A, 0x2F, 0x25, 0x22, 0x2E, 0x21);
  CMD(0x50, 0x37);
  CMD(0x60, 0x02, 0x02);
  CMD(0x61, 0x00, 0xA0, 0x01, 0x28);                  // 160 x 296
  CMD(0xE7, 0x1C);
  CMD(0xE3, 0x22);
  CMD(0xB4, 0xD0);
  CMD(0xB5, 0x03);
  CMD(0xE9, 0x01);
  CMD(0x30, 0x08);
}

// Mirror and flip_x make a slide appear upright on the tag as it is on the page.
// EL016: both false (v19 had both true and showed the splash and the slides turned by 180 degrees, checked on a tag 16 Nov 2026).
// EL022: scan lines in reverse order [?] (not rechecked on a tag since v19).
static const profile_t P_EL016 = { "EL016", 200, 200, false, false, 1, setup_el016 };
static const profile_t P_EL022 = { "EL022", 160, 296, true, false, 2, setup_el022 };

// ---- selection ----

bool panel_init(void)
{
  const hw_userdata_t *u = hw_userdata();
  prof = NULL;
  if ((u->ctrl == 0x1D || u->ctrl == 0x1C || u->ctrl == 0x37) && u->xres == 200 && u->yres == 200) {
    prof = &P_EL016;                                  // 0x1D seen on EL016F5W4C and EL016F6W4A, 0x37 on an older EL016; 0x1C is treated alike
  } else if (u->ctrl == 0x1E && u->xres == 296 && u->yres == 160) {
    prof = &P_EL022;                                  // seen on EL022F5C4C
  }
  pio_idle();
  return prof != NULL;
}

bool     panel_ok(void)          { return prof != NULL; }
uint16_t panel_w(void)           { return prof ? prof->w : 0; }
uint16_t panel_h(void)           { return prof ? prof->h : 0; }
uint16_t panel_line_bytes(void)  { return prof ? (uint16_t)(prof->w / 4) : 0; }
uint32_t panel_frame_bytes(void) { return prof ? (uint32_t)(prof->w / 4) * prof->h : 0; }
bool     panel_mirror(void)      { return prof && prof->mirror; }
bool     panel_flip_x(void)      { return prof && prof->flip_x; }
const char *panel_name(void)     { return prof ? prof->name : "?"; }

// ---- sequences ----

static void pulse(void)
{
  pio_wait_ms(200);
  pio_rst(false);
  pio_wait_ms(40);
  pio_rst(true);
  pio_wait_ms(200);
}

static void wake_and_setup(void)
{
  pio_wait_ms(20);
  pio_power_on();
  pio_wait_ms(100);                                   // let the supply settle
  pulse();
  if (prof->resets == 2) {
    pulse();
    pio_wait_ms(10);
  }
  prof->setup();
  if (prof->resets == 2) {
    pio_wait_ms(300);
  }
}

static void power_down(void)
{
  pio_wait_ms(100);
  CMD(0x02, 0x00);                                    // power off
  pio_wait_ms(100);
  (void)pio_wait_busy(2000, 1);
  CMD(0x07, 0xA5);                                    // deep sleep
  pio_wait_ms(100);
  pio_release();
}

bool panel_draw(panel_line_fn fn)
{
  if (!prof) {
    return false;
  }
  uint16_t lb = (uint16_t)(prof->w / 4);
  bool all = true;

  wake_and_setup();
  pio_cmd(0x10, NULL, 0, true);                       // start of the frame, chip select stays low
  for (uint16_t y = 0; y < prof->h; y++) {
    if (!fn(y, line)) {
      memset(line, 0x55, lb);                         // white
      all = false;
    }
    pio_data(line, lb);
  }

  pio_wait_ms(10);
  CMD(0x04, 0x00);                                    // power on the panel rails
  pio_wait_ms(100);
  (void)pio_wait_busy(1000, 1);
  pio_wait_ms(10);
  CMD(0x12, 0x00);                                    // refresh
  pio_wait_ms(300);
  (void)pio_wait_busy(50000, 1);                      // 20 to 24 s
  power_down();
  return all;
}

bool panel_probe_temp(uint8_t out[3])
{
  if (!prof || !out) {
    return false;
  }
  wake_and_setup();
  CMD(0x04, 0x00);
  pio_wait_ms(100);
  (void)pio_wait_busy(1000, 1);
  CMD(0x41, 0x00);                                    // temperature sensor on
  pio_wait_ms(100);
  out[0] = out[1] = out[2] = 0;
  pio_cmd(0x40, NULL, 0, true);                       // temperature read
  pio_read(out, 3);
  power_down();
  return true;
}
