// host fake of em_device.h: the register types that hw.h mentions
#ifndef FAKE_DEV_H
#define FAKE_DEV_H
typedef struct { int unused; } USART_TypeDef;
typedef struct { int unused; } I2C_TypeDef;
extern USART_TypeDef fake_usart[2];
extern I2C_TypeDef fake_i2c[1];
#define USART0 (&fake_usart[0])
#define USART1 (&fake_usart[1])
#define I2C0   (&fake_i2c[0])
#endif
