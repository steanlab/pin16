// pin16: system information: unique chip ID, reset cause, watchdog, battery voltage (also the lowest value
// during a panel draw). Own code.
#ifndef SYSINFO_H
#define SYSINFO_H
#include <stdbool.h>
#include <stdint.h>

void     sysinfo_early(void);                 // first thing after reset: remembers the reset cause (hw_init clears it)
void     sysinfo_start(void);                 // starts the watchdog and its keep-alive timer
void     sysinfo_poll(void);                  // main loop: feeds the watchdog
bool     sysinfo_pending(void);               // keep-alive due: do not sleep
uint32_t sysinfo_stack_free(uint32_t *total);  // bytes of the stack never used since start-up (watermark), total size in *total
uint32_t sysinfo_reset_cause(void);           // EMU_RSTCAUSE bits at the last start
void     sysinfo_uid(uint32_t *hi, uint32_t *lo);   // 64-bit EUI of the chip

uint16_t sysinfo_vbat(void);                  // supply voltage now, mV (0 = no reading)
void     sysinfo_load_begin(void);            // a panel draw starts: sample the supply, keep the watchdog fed
void     sysinfo_load_end(void);
uint16_t sysinfo_min_load(void);              // lowest voltage seen during the last draw, 0 = no draw yet
uint16_t sysinfo_min_ever(void);              // lowest voltage seen during any draw since power-up, 0 = none
uint8_t  sysinfo_percent(uint16_t mv);        // rough charge of a CR2450 from the unloaded voltage
bool     sysinfo_low(uint16_t mv);            // battery nearly empty

uint32_t sysinfo_draws(void);                 // finished panel draws since power-up
void     sysinfo_draw_done(void);
#endif
