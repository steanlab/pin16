// pin16: picture slots in the external flash and the upload protocol (own code).
// Area 0x3C000..0x7CFFF in 4 KiB sectors, first fit. A slot is a run of sectors, its first 256 bytes are the header:
//  0..3 'BG22', 4 layout version (1), 5 format (0 raw, one frame, 1 LZ4 block, 2 LZ4 block of the line-XORed frame),
//  6 slot number, 7 sectors in the run, 8..11 stored length LE, 12..15 CRC-32 of the stored stream, 16..19 serial.
// The header is written last, so an interrupted upload leaves no slot. The directory is rebuilt from the headers.
#ifndef SLOT_H
#define SLOT_H
#include <stdbool.h>
#include <stdint.h>
#include "panel.h"

#define SLOT_N        32
#define SLOT_BEG      0x3C000UL
#define SLOT_END      0x7D000UL        // 0x7D000..0x7EFFF settings, 0x7F000 self test
#define SLOT_SECT     4096UL
#define SLOT_NSECT    ((SLOT_END - SLOT_BEG) / SLOT_SECT)    // 65
#define SLOT_HDR      256UL
#define FRAME_BYTES   panel_frame_bytes()          // 2 bits per pixel: 10 000 bytes on EL016 (200 x 200), 11 840 on EL022 (160 x 296)
#define SLOT_MAXLEN   (panel_frame_bytes() + 100UL) // largest stored stream (a bit more than the frame for incompressible data)

#define CMD_SLOT_LIST   0x40
#define CMD_SLOT_ERASE  0x41
#define CMD_SLOT_UP     0x42
#define CMD_SLOT_DATA   0x43
#define CMD_SLOT_END    0x44
#define CMD_SLOT_VERIFY 0x47
#define CMD_SLOT_MTU    0x48
#define CMD_SLOT_DRAW   0x45
#define CMD_SLOT_ABORT  0x49
#define CMD_SLOT_NEXT   0x4C

bool slot_is_cmd(uint8_t c);
void slot_cmd(const uint8_t *d, uint8_t len);   // answers with text lines (ble_badge_say)
bool slot_busy(void);
void slot_poll(void);                            // main loop: runs a requested draw
bool slot_draw_pending(void);
void slot_request_next_tap(void);                /* tap: draw the next slot, then open the Bluetooth window */
void slot_request_next(void);                    // draw the next filled slot
void slot_request_draw(int s, bool od);          // draw slot s from the main loop (od: answer with 00 73 / 00 74)
bool slot_request_pic(uint8_t kind, bool boot);  // test picture / splash (pics.h) from the main loop; boot: open the power-up window afterwards. false = refused
bool slot_request_clean(void);
bool slot_dump_start(int s);                     // send the stored stream of slot s as binary frames [C0 slot seq_lo seq_hi data...], then the line "DUMP done"
bool slot_dump_active(void);                   // panel clean cycle: black, white, red, yellow, white (about 2 minutes)
void slot_show_arm(void);                        // (re)start the slideshow timer from the settings
int  slot_next_preview(void);
int  slot_current(void);
// OpenDisplay direct write into the slot g_set.od_slot (frame of FRAME_BYTES bytes, format 0)
bool slot_od_begin(void);
bool slot_od_data(const uint8_t *d, uint32_t n);
bool slot_od_end(void);                            // an upload is running
void slot_link_closed(void);                     // the link ended: drop a running upload

// For the display code (next step)
bool slot_valid(int s);
bool slot_info(int s, uint32_t *data_addr, uint32_t *len, uint8_t *fmt);
#endif
