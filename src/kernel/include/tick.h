#ifndef _TICK_H
#define _TICK_H

#include <stdint.h>

#include "sync.h"

extern osslock_t ticksLock;

void tickInit(void);
void tickIntHandler(void);
/* 系统TICK是唯一的，既系统暴露给延时函数等的TICK值是唯一的，即核0上的tick计数值。 */
uint64_t getOSTick(void);
/* 多核都有独立的定时器中断和tick计数,getCurrentTick()返回当前core的tick计数。 */
uint64_t getCurrentTick(void);
void setOSTick(uint64_t tick);
/* 核忙等待延时，以tick为单位。 */
void delay(uint64_t ticks);

#endif