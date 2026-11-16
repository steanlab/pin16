#ifndef FAKE_BTL_H
#define FAKE_BTL_H
#include <stdint.h>
typedef struct { uint32_t version, capabilities; } BootloaderInformation_t;
void bootloader_getInfo(BootloaderInformation_t *i);
void bootloader_rebootAndInstall(void);
#endif
