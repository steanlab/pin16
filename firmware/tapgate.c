// pin16: protection of dangerous commands by physical presence, see tapgate.h. Own code.
#include "sl_sleeptimer.h"
#include "tapgate.h"

static uint64_t touched_tick = 0;
static bool     have = false;

void tapgate_touch(void)
{
  touched_tick = sl_sleeptimer_get_tick_count64();     // 64 bit: the 32-bit counter wraps every 36 hours
  have = true;
}

static uint64_t age_ms(void)
{
  uint64_t ms = 0;
  sl_sleeptimer_tick64_to_ms(sl_sleeptimer_get_tick_count64() - touched_tick, &ms);
  return ms;
}

bool tapgate_open(void)
{
  return have && age_ms() < TAPGATE_MS;
}

uint32_t tapgate_left_s(void)
{
  if (!tapgate_open()) {
    return 0;
  }
  return (uint32_t)((TAPGATE_MS - age_ms() + 999) / 1000);
}
