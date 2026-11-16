// pin16: the tag hardware: pins, factory data, inputs. Own code.
// The pin table was read from the tags themselves (hardware cards of EL016F5W4C, EL016F6W4A and EL022F5C4C, see docs/hardware.md);
// it is the same for all three. Panel geometry and controller type come from the Solum factory data page ("userdata").
#ifndef HW_H
#define HW_H

#include <stdbool.h>
#include <stdint.h>
#include "em_device.h"
#include "em_gpio.h"

#define HW_NONE  0xFF                        // port value of a pin that does not exist

typedef struct { uint8_t port; uint8_t pin; } hw_pin_t;

typedef struct {
  USART_TypeDef *usart;
  hw_pin_t mosi, miso, sck, cs;
} hw_flash_t;                                // external flash on USART0

typedef struct {
  USART_TypeDef *usart;
  hw_pin_t mosi, sck, cs, dc, rst, busy, en; // panel on USART1, no MISO; BUSY low = busy; EN = panel power (high = on)
} hw_panel_t;

typedef struct {
  I2C_TypeDef *i2c;
  hw_pin_t sda, scl, pwr, fd;                // pwr: supply of the chip for I2C access, fd: field detect (high = a reader is near)
} hw_nfc_t;

typedef struct {
  hw_flash_t flash;
  hw_panel_t panel;
  hw_nfc_t   nfc;
  hw_pin_t   btn[2];                         // PB1, PA5 (which one is on the case differs between models), active low
  hw_pin_t   led[3];                         // as the eye sees it: red, green, blue; active low
  hw_pin_t   dbg_tx, dbg_rx;                 // debug UART
} hw_tag_t;

extern const hw_tag_t hw_pins;

// Factory data page of the Solum tag, read once.
typedef struct {
  uint8_t  ctrl;                             // panel controller type (byte 0x09)
  uint8_t  color;                            // colour type (0x0A)
  uint16_t xres, yres;                       // resolution as stored (0x0B, 0x0D); the panel scans yres lines of xres pixels or the other way round
  uint8_t  capa0, capa1;                     // capability bits (0x12, 0x13)
  uint8_t  tag;                              // tag type (0x16), the "tag type" shown by the page
} hw_userdata_t;

const hw_userdata_t *hw_userdata(void);

#define HW_IN_NFC   1                        // a reader came near (field detect rose)
#define HW_IN_BTN1  2
#define HW_IN_BTN2  3
typedef void (*hw_input_cb_t)(uint8_t src);

void   hw_init(void);                        // pins to their idle state, debug UART, NFC chip probe, power manager hooks
void   hw_inputs_start(hw_input_cb_t cb);    // interrupts of the buttons and of the NFC field detect
int8_t hw_temperature(void);                 // chip temperature, degrees C
uint8_t hw_tag_type(void);                   // userdata byte 0x16

#endif
