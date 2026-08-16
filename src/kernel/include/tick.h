#ifndef _TICK_H
#define _TICK_H

#include <stdint.h>

#include "sync.h"

extern osslock_t tick_lock;

void tick_init(void);
void tick_int_handler(void);
/* 系统TICK是唯一的，既系统暴露给延时函数等的TICK值是唯一的，即核0上的tick计数值。 */
uint64_t tick_get_os_tick(void);
/* 多核都有独立的定时器中断和tick计数,tick_get_current()返回当前core的tick计数。 */
uint64_t tick_get_current(void);
void tick_set_os_tick(uint64_t tick);
/* 核忙等待延时，以tick为单位。 */
void tick_delay(uint64_t ticks);

#endif