// pin16: LED engine (own code). Hardware PWM on TIMER1, three channels, mixing, flashes and pulses.
// Runs from one sleeptimer; nothing waits. A new pattern replaces a running one.
#ifndef LED_H
#define LED_H
#include <stdbool.h>
#include <stdint.h>

// Colours as the eye sees them on this tag (the tag config names its pins differently, see led.c)
enum { LC_OFF = 0, LC_GREEN, LC_BLUE, LC_RED, LC_AMBER, LC_CYAN, LC_MAGENTA, LC_WHITE };

void led_init(void);                                                    // after hw_init()
void led_flash(uint8_t col, uint8_t n, uint16_t on_ms, uint16_t gap_ms); // n flashes at full brightness
void led_flash_dim(uint8_t col, uint8_t n, uint16_t on_ms, uint16_t gap_ms, uint8_t peak);
void led_pulse(uint8_t col, uint16_t period_ms, uint8_t peak);          // slow breathing until led_stop()
void led_pulse_bg(uint8_t col, uint16_t period_ms, uint8_t peak);       // background breathing (beacon): any other pattern replaces it
bool led_bg_running(void);
void led_stop(void);                                                    // all off, pattern ended
bool led_pulsing(void);                                                 // a foreground pulse runs
bool led_busy(void);                                                    // any pattern (flash, pulse, steady test colour) is running
void led_set_amber(uint8_t g);                                          // green part of amber 0..255 (default 150, higher = more yellow)
uint8_t led_get_amber(void);
void led_rgb(uint8_t r, uint8_t g, uint8_t b, uint16_t ms);             // test: steady colour for ms (0 = until led_stop)
#endif
