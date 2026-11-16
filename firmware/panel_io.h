// pin16: lowest layer of the panel: pins, SPI, waiting. Own code.
// The panel code (panel.c) only calls these functions, so a PC test can replace this file and record what the panel would see.
#ifndef PANEL_IO_H
#define PANEL_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void pio_idle(void);                                  // all panel pins in their off state (called once at start-up)
void pio_power_on(void);                              // panel supply on, pins and SPI set up
void pio_release(void);                               // pins released (inputs, pull-down), supply off, SPI closed
void pio_wait_ms(uint32_t ms);                        // sleep that long (the chip sleeps in between)
void pio_rst(bool high);                              // level of the reset pin
// Command byte, then (if n > 0) the data bytes after a short chip-select pulse. keep_cs: leave chip select low afterwards.
void pio_cmd(uint8_t op, const uint8_t *data, uint8_t n, bool keep_cs);
void pio_data(const uint8_t *data, size_t n);         // data bytes, chip select low and left low
void pio_read(uint8_t *data, size_t n);               // reads n bytes after a command that was sent with keep_cs
// Waits until BUSY reads `level`. The panel pulls BUSY low while it works. Returns the time waited in ms.
uint32_t pio_wait_busy(uint32_t timeout_ms, uint8_t level);

#endif
