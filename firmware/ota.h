// pin16: firmware update over Bluetooth (own code).
// The page uploads a GBL file (build artifact ota_files/pin16_vN.ota.bin) into the bootloader slot 0..0x3B000 of the
// external flash. The tag checks the file (length, CRC-32 read back, GBL header and end CRC, product mark BG22BADG), and only
// on the command "install" restarts into the Gecko bootloader, which flashes the application.
#ifndef OTA_H
#define OTA_H
#include <stdbool.h>
#include <stdint.h>

#define CMD_OTA 0x46          // 46 00 status | 01 begin | 02 data | 03 end/verify | 04 A5 install | 05 abort

bool ota_is_cmd(uint8_t c);
void ota_cmd(const uint8_t *d, uint8_t len);      // answers with text lines
bool ota_busy(void);                              // an upload or a verified image is waiting
void ota_link_closed(void);                       // the link ended: drop it
#endif
