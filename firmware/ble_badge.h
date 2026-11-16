// pin16: Bluetooth LE module (own code)
#ifndef BLE_BADGE_H
#define BLE_BADGE_H
#include <stdbool.h>
#include <stdint.h>

// Connectable window length after power-up and after every NFC tap / button press.
#define BLE_WINDOW_MS        (120UL * 1000UL)
// After a link ends the window is open for this long again.
#define BLE_AFTER_MS         (60UL * 1000UL)
// A link on which the client sends no command at all within this time is closed (stale link of the phone).
// The timer restarts when the client enables notifications (the descriptor write).
#define BLE_FIRST_MS         (30UL * 1000UL)
// A connection with no traffic for this long is closed by the badge.
#define BLE_IDLE_MS          (25UL * 1000UL)

// Protocol (see docs/PROTOCOL.md). Request: [cmd, payload...]. Response: [cmd|0x80, status, payload...]
#define BCMD_PING   0x01
#define BCMD_INFO   0x02
#define BCMD_ECHO   0x03
#define BCMD_LED    0x04
#define BCMD_FJEDEC 0x10   // external flash: JEDEC id
#define BCMD_FREAD  0x11   // [addr hi, mid, lo, len<=200] read bytes
#define BCMD_FTEST  0x12   // erase/program/verify self test of the last sector

#define BSTAT_OK       0x00
#define BSTAT_BADCMD   0xFF
#define BSTAT_BADLEN   0xFE

// Event log ids (command 4F 09)
enum { EV_BOOT = 1, EV_OPEN, EV_CLOSE, EV_MTU, EV_CHAR, EV_RX, EV_PARAM, EV_ADV, EV_ADVTMO, EV_WIN, EV_FIXLAT, EV_RSSI, EV_IDLE, EV_BEACON, EV_ERR };
void ble_badge_ev(uint8_t id, uint16_t a, uint16_t b);

// Text line to the client (one notification, cut to MTU-3). printf style, at most 63 characters.
void ble_badge_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ble_badge_send(const uint8_t *p, uint8_t n);   // raw bytes as one notification (at most 64)
bool ble_badge_room(void);             // the line queue can take more
bool ble_badge_link_ready(void);       // connected with notifications on
void ble_badge_led_work(uint8_t what);  // LED for panel work: 0 starts (amber pulse), 1 finished well, 2 failed
void ble_badge_touch(void);            // count now as activity (after a long blocking job)
void ble_badge_close_link(void);       // close the current link now
void ble_badge_close_link_and_window(void);   // close the link and do not open the window again (BTHome starts at once)
void ble_badge_dump_events(void);      // text lines of the event log
void ble_badge_apply_tx(void);         // radio power from the settings
void ble_badge_reboot_install(void);   // restart into the bootloader, which installs the firmware image staged by ota.c
void ble_badge_reboot(void);           // restart in about 0.5 s, after the queue is sent
const char *ble_badge_od_name(void);   // name advertised in OpenDisplay mode ("OD" + 3 address bytes in hex)
uint16_t ble_badge_mtu(void);          // ATT MTU of the current link (23 until the phone asks for more)

void ble_badge_request_window(void);   // safe from interrupt / callback context
void ble_badge_request_boot_window(void);   // the window of the power-up length (after the boot splash)
void ble_badge_process(void);          // call from the main loop
bool ble_badge_event_pending(void);    // true while the main loop has work to do
bool ble_badge_is_active(void);        // window open or connected

#endif
