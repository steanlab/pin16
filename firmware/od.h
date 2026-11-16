// pin16: OpenDisplay subset (messages start with 0x00; answers are binary: status cmd data). Own code.
#ifndef OD_H
#define OD_H
#include <stdbool.h>
#include <stdint.h>
void od_rx(const uint8_t *d, uint8_t len);   // one message from the client
bool od_muted(void);                         // an OpenDisplay client is on this link: no text lines
void od_link_down(void);
void od_adv_data(uint8_t adv_set);           // OpenDisplay mode: advertisement = flags, service 0x2446, 16-byte record; scan response = name
// Layout of the reference firmware (py-opendisplay decode_msd_payload): 0..1 company id, 2..12 dynamic data (0xFF = none), 13 temperature
// (T + 40) * 2, 14 battery in units of 10 mV (low 8 bits), 15 status: bit 0 battery bit 8, bit 1 reboot flag, bit 2 connection requested,
// bit 3 encryption on, bits 4..7 loop counter.
static inline void od_msd_pack(uint8_t b[16], uint16_t mv, int tc, uint8_t loop)
{
  uint32_t v = ((uint32_t)mv + 5u) / 10u;
  int t2 = (tc + 40) * 2;
  if (v > 0x1FFu) v = 0x1FFu;
  if (t2 < 0) t2 = 0;
  if (t2 > 255) t2 = 255;
  b[0] = 0x46; b[1] = 0x24;
  for (int i = 2; i <= 12; i++) b[i] = 0xFF;
  b[13] = (uint8_t)t2;
  b[14] = (uint8_t)v;
  b[15] = (uint8_t)(((v >> 8) & 1u) | ((unsigned)(loop & 15u) << 4));
}
void od_msd(uint8_t b[16]);                  // the 16-byte record (company id 0x2446 first): answer of 00 44 and the advertised data
#endif
