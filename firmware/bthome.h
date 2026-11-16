// pin16: optional BTHome v2 beacon (readable by Home Assistant and other BTHome clients). Own code.
// Off by default. Sent as a short burst of non-connectable advertisements every 10 / 30 / 60 / 300 s, and only while no Bluetooth
// window is open and no link exists, so it never competes with a connection. Unencrypted.
#ifndef BTHOME_H
#define BTHOME_H
#include <stdbool.h>
#include <stdint.h>

void bthome_init(const char *name);        // after the stack has booted: creates the advertising set
void bthome_apply(void);                   // settings changed: (re)starts or stops the beacon
void bthome_on_timeout(uint8_t handle);    // advertiser timeout event
void bthome_quiet(void);                   // a window opens or a link is made: stop a running burst
bool bthome_pending(void);                 // a beacon is due: do not sleep
void bthome_process(void);                 // main loop
void bthome_kick(void);                    // send a beacon at once (the window has just closed)
void bthome_window_data(uint8_t adv_set);  // before a window starts: advertising data of the connectable set (optionally with BTHome data)
#endif
