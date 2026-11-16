// BG22Badge bootloader: pins and clock of the external flash on the Solum tags (USART0). Own file, GPL-3.0-or-later.
// Same wiring on EL016 (1.6") and EL022 (2.2"): MISO PC0, MOSI PC1, SCK PC2, CS PC3. The flash has no enable, hold or write protect pin.
#ifndef BTL_SPI_CONTROLLER_USART_DRIVER_CONFIG_H
#define BTL_SPI_CONTROLLER_USART_DRIVER_CONFIG_H

#define SL_USART_EXTFLASH_FREQUENCY              6400000

#define SL_USART_EXTFLASH_PERIPHERAL             USART0
#define SL_USART_EXTFLASH_PERIPHERAL_NO          0

#define SL_USART_EXTFLASH_TX_PORT                gpioPortC
#define SL_USART_EXTFLASH_TX_PIN                 1

#define SL_USART_EXTFLASH_RX_PORT                gpioPortC
#define SL_USART_EXTFLASH_RX_PIN                 0

#define SL_USART_EXTFLASH_CLK_PORT               gpioPortC
#define SL_USART_EXTFLASH_CLK_PIN                2

#define SL_USART_EXTFLASH_CS_PORT                gpioPortC
#define SL_USART_EXTFLASH_CS_PIN                 3

#endif
