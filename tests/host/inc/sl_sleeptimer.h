// host fake: virtual-time sleeptimer with the semantics of GSDK 4.4 (start while running = error, one-shot removed before callback)
#ifndef FAKE_SLEEPTIMER_H
#define FAKE_SLEEPTIMER_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef uint32_t sl_status_t;
#define SL_STATUS_OK 0u
#define SL_STATUS_NOT_READY 0x0Au
#define SL_STATUS_INVALID_STATE 0x02u
#define SL_STATUS_INVALID_PARAMETER 0x21u
typedef struct sl_sleeptimer_timer_handle sl_sleeptimer_timer_handle_t;
typedef void (*sl_sleeptimer_timer_callback_t)(sl_sleeptimer_timer_handle_t *h, void *data);
struct sl_sleeptimer_timer_handle { bool run; uint64_t due_us; uint32_t period_ms; sl_sleeptimer_timer_callback_t cb; void *data; };
#define SL_TICK_HZ 32768ULL
sl_status_t sl_sleeptimer_start_timer_ms(sl_sleeptimer_timer_handle_t *h, uint32_t ms, sl_sleeptimer_timer_callback_t cb, void *d, uint8_t prio, uint16_t fl);
sl_status_t sl_sleeptimer_start_periodic_timer_ms(sl_sleeptimer_timer_handle_t *h, uint32_t ms, sl_sleeptimer_timer_callback_t cb, void *d, uint8_t prio, uint16_t fl);
sl_status_t sl_sleeptimer_stop_timer(sl_sleeptimer_timer_handle_t *h);
uint32_t sl_sleeptimer_get_tick_count(void);
uint64_t sl_sleeptimer_get_tick_count64(void);
uint32_t sl_sleeptimer_tick_to_ms(uint32_t t);
sl_status_t sl_sleeptimer_tick64_to_ms(uint64_t t, uint64_t *ms);
// test control
void vt_advance_ms(uint64_t ms);       // run the clock, firing timers in order
void vt_set_ms(uint64_t ms);           // jump the clock (no timers fire) e.g. to just below a wrap
uint64_t vt_now_us(void);
int  vt_running_timers(void);
extern int vt_start_errors;            // number of start calls that failed because the handle was running
#endif
