// pin16: access to the external SPI flash (Fudan, JEDEC A1 28 13, 512 KB). Own code.
// Pins and the SPI unit come from the tag hardware table (hw.c).
#ifndef EXT_FLASH_H
#define EXT_FLASH_H
#include <stdbool.h>
#include <stdint.h>

#define XF_SIZE         0x80000UL      // 512 KB
#define XF_SECTOR       4096UL
#define XF_PAGE         256UL
// Writes and erases are refused below this address: 0..0x3B000 is the bootloader OTA slot.
#define XF_WRITE_MIN    0x3C000UL
#define XF_TEST_SECTOR  0x7F000UL      // last sector, used by the self test
#define XF_OTA_END      0x3B000UL      // end of the bootloader OTA slot (236 KB from address 0)

void xf_ota_mode(bool on);             // on: writes and erases are allowed in the OTA slot 0..0x3B000 and refused elsewhere (firmware update only)
bool xf_begin(uint8_t jedec[3]);       // power up the SPI and the flash, read the JEDEC id
bool xf_sfdp(uint32_t addr, uint8_t *buf, uint32_t len);   // SFDP table bytes (command 5A), flash must be open; diagnostics
void xf_end(void);                     // flash into deep power down, SPI off
void xf_read(uint32_t addr, uint8_t *buf, uint32_t len);
bool xf_erase_sector(uint32_t addr);   // 4 KB, address inside the writable area
bool xf_write(uint32_t addr, const uint8_t *data, uint32_t len);   // erased area only, any length

// Self test of the last sector: erase, blank check, program 256 B, verify, erase.
// Returns 0 = ok, 1 = no flash, 2 = erase failed, 3 = not blank, 4 = program failed, 5 = verify mismatch.
uint8_t xf_selftest(uint16_t *erase_ms, uint16_t *prog_ms, uint16_t *bad_index);

#endif
