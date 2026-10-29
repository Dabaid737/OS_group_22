#ifndef __KERN_DRIVER_CLOCK_H__
#define __KERN_DRIVER_CLOCK_H__

#include <defs.h>

extern volatile size_t ticks;
//这是声明，让包含 clock.h 的其他文件能够使用同一个全局变量。所以你在 trap.c 中修改 ticks，修改的就是这里定义的计数器
void clock_init(void);
void clock_set_next_event(void);

#endif /* !__KERN_DRIVER_CLOCK_H__ */

