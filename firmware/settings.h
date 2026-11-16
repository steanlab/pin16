// pin16: persistent settings in two flash sectors (0x7D000, 0x7E000), 32-byte records appended one after another,
// newest valid record wins, the other sector is erased when one is full. Own code.
#ifndef SETTINGS_H
#define SETTINGS_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint8_t  ss_en;        // slideshow on
  uint16_t ss_every;     // minutes 1..1080
  uint32_t ss_mask;      // bit n = slot n takes part, 0 = every filled slot
  uint8_t  orient;       // bit0 reverse the lines (raw slots only), bit1 reverse the pixels of a line
  uint8_t  od_slot;      // slot that takes the OpenDisplay stream
  uint8_t  lut;          // 4 x 2 bits: output colour for stored code 0..3 (default 0xE4 = unchanged)
  uint8_t  txp;          // radio power index 0..5
  uint8_t  led;          // 0 off, 1 errors, 2 all events
  uint8_t  opmode;       // 0 standalone (windows), 1 OpenDisplay (always connectable)
  uint8_t  wt;           // window after a tap / a closed link, seconds 10..250
  uint8_t  wb;           // window after power-up, seconds 10..250
  uint8_t  tap_next;     // 1 = a tap also draws the next slide
  uint8_t  clean_n;      // reserved (clean the panel before every n-th slide)
  uint8_t  clean_mode;
  char     name[6];      // name tail, empty = from the address
  uint8_t  bt_en;        // BTHome beacon on (only while no window is open)
  uint8_t  bt_itv;       // beacon interval: 0 = 10 s, 1 = 30 s, 2 = 60 s, 3 = 300 s
  uint8_t  bt_mask;      // beacon fields, bit 0 battery %, 1 temperature, 2 voltage, 3 lowest voltage under load, 4 slide, 5 draws, 6 battery low
  uint8_t  splash_off;   // 1 = no boot splash
  uint8_t  bt_win;       // 1 = BTHome data also in the connectable advertisement while a window is open (experimental)
  uint8_t  ss_order;     // 0 = in order, 1 = random
  uint8_t  amber_g;      // green part of the amber LED colour 0..255 (default 150)
  uint8_t  bc_preset;    // beacon / locator preset 0 = none, 1 locator, 2 heartbeat, 3 SOS, 4 rainbow, 5 breathing
  uint8_t  bc_col;       // beacon colour 1..7 (LC_*)
  uint8_t  bc_lvl;       // beacon brightness 1..4
  uint8_t  bc_dur;       // beacon duration index 0..4 = 1 h, 6 h, 24 h, 72 h, 7 days (limited per preset)
  uint8_t  bc_keep;      // 1 = the beacon starts again after a restart (not after a crash or low voltage)
} settings_t;

extern settings_t g_set;

void     settings_load(void);      // at boot, opens and closes the flash itself
bool     settings_save(void);      // false = flash busy or write failed
uint32_t settings_seq(void);       // number of saves so far (record sequence)
#endif
