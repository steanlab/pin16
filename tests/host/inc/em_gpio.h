#ifndef FAKE_GPIO_H
#define FAKE_GPIO_H
#include <stdint.h>
typedef uint8_t GPIO_Port_TypeDef;
enum { gpioPortA = 0, gpioPortB, gpioPortC, gpioPortD };
typedef enum { gpioModeDisabled, gpioModeInput, gpioModeInputPull, gpioModePushPull } GPIO_Mode_TypeDef;
#define gpioPortInvalid ((GPIO_Port_TypeDef)0xFF)
#define _GPIO_TIMER_CC0ROUTE_PORT_SHIFT 0
#define _GPIO_TIMER_CC0ROUTE_PIN_SHIFT 16
typedef struct { volatile uint32_t ROUTEEN, CC0ROUTE, CC1ROUTE, CC2ROUTE; } GPIO_TIMERROUTE_t;
typedef struct { GPIO_TIMERROUTE_t TIMERROUTE[2]; } GPIO_TypeDef;
extern GPIO_TypeDef gpio_regs;
#define GPIO (&gpio_regs)
void GPIO_PinOutSet(GPIO_Port_TypeDef p, unsigned pin);
void GPIO_PinOutClear(GPIO_Port_TypeDef p, unsigned pin);
void GPIO_PinModeSet(GPIO_Port_TypeDef p, unsigned pin, GPIO_Mode_TypeDef m, unsigned out);
extern int gpio_cs_low;     // CS asserted (flash sim)
extern uint8_t gpio_out[4][16];
#endif
