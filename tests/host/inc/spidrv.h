#ifndef FAKE_SPIDRV_H
#define FAKE_SPIDRV_H
#include <stdint.h>
#include "em_gpio.h"
typedef uint32_t Ecode_t;
#define ECODE_EMDRV_SPIDRV_OK 0
typedef struct { int open; } SPIDRV_HandleData_t;
typedef SPIDRV_HandleData_t *SPIDRV_Handle_t;
typedef struct { void *port; GPIO_Port_TypeDef portTx, portRx, portClk; uint8_t pinTx, pinRx, pinClk; uint32_t bitRate; int csControl; } SPIDRV_Init_t;
#define SPIDRV_MASTER_DEFAULT { 0 }
#define spidrvCsControlApplication 1
Ecode_t SPIDRV_Init(SPIDRV_Handle_t h, SPIDRV_Init_t *i);
Ecode_t SPIDRV_DeInit(SPIDRV_Handle_t h);
Ecode_t SPIDRV_MTransmitB(SPIDRV_Handle_t h, const void *b, int n);
Ecode_t SPIDRV_MReceiveB(SPIDRV_Handle_t h, void *b, int n);
Ecode_t SPIDRV_MTransferB(SPIDRV_Handle_t h, const void *tx, void *rx, int n);
extern int spi_open_count, spi_init_calls, spi_deinit_calls;
#endif
