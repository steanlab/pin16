#ifndef FAKE_H
#define FAKE_H
#include <stdint.h>
#include <stdbool.h>
extern int core_depth_at_isr, core_depth, core_max_depth, pm_em1, pm_em1_max, cmu_timer1, timer_enabled;
extern uint32_t timer_cmp[3];
extern uint8_t fl[524288];
extern uint32_t fl_erase_cnt[128];
extern int fl_violations, fl_no_wel, fl_prog_ops, fl_erase_ops, fl_read_when_down, fl_cut_budget, fl_busy_reads, fl_stuck, spi_open_count, spi_init_calls, spi_deinit_calls, btl_reboot_install;
extern bool fl_dead, fl_powered_down;
extern uint8_t fl_jedec[3];
extern uint32_t fl_min_write, fl_below_slots_ops;
extern long udelay_total_us;
extern void (*vt_hook)(void);
extern uint8_t gpio_out[4][16];
void fl_reset(void);
// stubs of other modules
extern uint16_t stub_vbat, stub_mtu; extern int stub_slot_busy, stub_draw_pending, stub_reboot_calls, stub_hold_calls;
extern char say_last[96]; extern int say_count; extern char say_log[256][96]; extern int say_log_n;
#include "hw.h"
extern hw_userdata_t fake_ud;
#endif
