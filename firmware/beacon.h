// pin16: beacon / locator: the LED plays a pattern for a limited time (own code).
// Limits that protect the battery: duration cap per preset, brightness cap per preset, short flashes, pause while the panel is
// drawn, supply check every 30 s (stops below 2.7 V), no automatic start after a crash or low voltage.
#ifndef BEACON_H
#define BEACON_H
#include <stdbool.h>
#include <stdint.h>

enum { BC_NONE = 0, BC_LOCATOR, BC_HEARTBEAT, BC_SOS, BC_RAINBOW, BC_BREATH };

// Starts a pattern. The values are limited to what the preset allows. false = refused (battery low, no LED).
bool beacon_start(uint8_t preset, uint8_t col, uint8_t lvl, uint8_t dur_idx);
void beacon_stop(void);
bool beacon_active(void);
uint8_t beacon_preset(void);
uint32_t beacon_left_s(void);              // seconds until it switches itself off
uint8_t beacon_max_lvl(uint8_t preset);
uint8_t beacon_max_dur(uint8_t preset);
void beacon_hold(bool hold);               // a panel draw or a firmware update runs: no flashes
bool beacon_pending(void);                 // the 30 s check is due: do not sleep
void beacon_poll(void);                    // main loop
void beacon_boot(bool bad_reset);          // after a restart: starts again if the settings ask for it
#endif
