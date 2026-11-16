#ifndef FAKE_CMU_H
#define FAKE_CMU_H
#include <stdbool.h>
typedef enum { cmuClock_TIMER1 = 1 } CMU_Clock_TypeDef;
extern int cmu_timer1;
void CMU_ClockEnable(CMU_Clock_TypeDef c, bool en);
#endif
