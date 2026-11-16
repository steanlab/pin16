// pin16: hooks of the diagnostic build ("hardware card"), see hwcard.c. Own code.
// Without BG_DIAG every hook is an empty statement: the normal firmware carries no trace code and no trace memory.
#ifndef HWCARD_H
#define HWCARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef BG_DIAG
void hwc_add(char type, const uint8_t *d0, size_t n0, const uint8_t *d1, size_t n1, uint32_t total);
uint8_t hwc_levels(void);                       // bit 0 BUSY, bit 1 RST, bit 2 DC
void hwc_clear(void);
void hwc_i2c(uint8_t addr, uint16_t flags, int ok, const uint8_t *b0, size_t n0, const uint8_t *b1, size_t n1);
void hwc_card_start(void);                      // 4F 0D / 4F 0F: the card and the trace as text lines
void hwc_poll(void);                            // main loop of the Bluetooth module: sends the next lines
bool hwc_busy(void);
#define HWC_I2C(a, f, ok, b0, n0, b1, n1) hwc_i2c((a), (f), (ok), (b0), (n0), (b1), (n1))
#define HWC_EVENT(t)            do { uint8_t lv_ = hwc_levels(); hwc_add((t), &lv_, 1, NULL, 0, 0); } while (0)   // A power on, N reset pin, Z released
#define HWC_CMD(op, d, n)       do { uint8_t o_ = (op); hwc_add('C', &o_, 1, (d), ((d) && (n) > 0) ? ((n) < 48 ? (n) : 48) : 0, (uint32_t)(n)); } while (0)
#define HWC_DATA(d, n)          hwc_add('D', NULL, 0, (d), (d) ? (((n) < 48) ? (n) : 48) : 0, (uint32_t)(n))
#define HWC_READ(d, n)          hwc_add('X', NULL, 0, (d), ((n) < 48) ? (n) : 48, (uint32_t)(n))
#define HWC_WAIT(ms)            hwc_add('W', NULL, 0, NULL, 0, (uint32_t)(ms))
#define HWC_BUSY_BEGIN(exp, to) do { uint8_t b_[2] = { (uint8_t)(exp), hwc_levels() }; hwc_add('B', b_, 2, NULL, 0, (uint32_t)(to)); } while (0)
#define HWC_BUSY_END(ms)        do { uint8_t lv_ = hwc_levels(); hwc_add('b', &lv_, 1, NULL, 0, (uint32_t)(ms)); } while (0)
#else
#define HWC_I2C(a, f, ok, b0, n0, b1, n1) ((void)0)
#define HWC_EVENT(t)            ((void)0)
#define HWC_CMD(op, d, n)       ((void)0)
#define HWC_DATA(d, n)          ((void)0)
#define HWC_READ(d, n)          ((void)0)
#define HWC_WAIT(ms)            ((void)0)
#define HWC_BUSY_BEGIN(exp, to) ((void)0)
#define HWC_BUSY_END(ms)        ((void)0)
#endif

#endif
