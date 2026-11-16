#ifndef FAKE_TIMER_H
#define FAKE_TIMER_H
#include <stdint.h>
#include <stdbool.h>
typedef struct { int dummy; } TIMER_TypeDef;
extern TIMER_TypeDef timer1_regs;
#define TIMER1 (&timer1_regs)
typedef struct { bool enable; int prescale; int mode; } TIMER_Init_TypeDef;
#define TIMER_INIT_DEFAULT { true, 0, 0 }
enum { timerPrescale128 = 7, timerModeUp = 0, timerCCModePWM = 3, timerOutputActionSet = 1, timerOutputActionClear = 2 };
typedef struct { int mode; int cofoa; int cmoa; bool outInvert; } TIMER_InitCC_TypeDef;
#define TIMER_INITCC_DEFAULT { 0, 0, 0, false }
extern int timer_enabled; extern uint32_t timer_cmp[3];
void TIMER_Init(TIMER_TypeDef *t, const TIMER_Init_TypeDef *i);
void TIMER_TopSet(TIMER_TypeDef *t, uint32_t v);
void TIMER_InitCC(TIMER_TypeDef *t, unsigned c, const TIMER_InitCC_TypeDef *i);
void TIMER_CounterSet(TIMER_TypeDef *t, uint32_t v);
void TIMER_Enable(TIMER_TypeDef *t, bool en);
void TIMER_CompareSet(TIMER_TypeDef *t, unsigned c, uint32_t v);
#endif
