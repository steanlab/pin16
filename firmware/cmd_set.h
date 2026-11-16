// pin16: settings commands 4A, 4B, 4D, 4E, 4F (own code)
#ifndef CMD_SET_H
#define CMD_SET_H
#include <stdbool.h>
#include <stdint.h>
bool cmdset_is(uint8_t c);
void cmdset_run(const uint8_t *d, uint8_t len);
#endif
