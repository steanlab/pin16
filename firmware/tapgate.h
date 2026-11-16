// pin16: protection of dangerous commands by physical presence. Own code.
// The commands that replace the firmware (OTA), rewrite the NFC chip or change how the tag can be reached over Bluetooth are only
// accepted within TAPGATE_MS after someone touched the tag with a phone (NFC field) or pressed a button. Slides, ordinary settings
// and everything else are not affected. The gate closes by itself; a reboot closes it too.
#ifndef TAPGATE_H
#define TAPGATE_H

#include <stdbool.h>
#include <stdint.h>

#define TAPGATE_MS  60000UL

void     tapgate_touch(void);        // called for every NFC tap and button press
bool     tapgate_open(void);         // true while the last touch is younger than TAPGATE_MS
uint32_t tapgate_left_s(void);       // seconds until the gate closes, 0 = closed

#endif
