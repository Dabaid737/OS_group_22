#include <clock.h>
#include <defs.h>
#include <sbi.h>
#include <stdio.h>
#include <riscv.h>

volatile size_t ticks;
//全局变量 要求编译器按相应规则保留对变量的实际访问，不能假设它一直不变
//普通执行流可能正在检查 ticks，而中断处理代码会改变它。编译器不能只读一次，就一直使用旧值
static inline uint64_t get_cycles(void) {
#if __riscv_xlen == 64
    uint64_t n;
    __asm__ __volatile__("rdtime %0" : "=r"(n));
    return n;
#else
    uint32_t lo, hi, tmp;
    __asm__ __volatile__(
        "1:\n"
        "rdtimeh %0\n"
        "rdtime %1\n"
        "rdtimeh %2\n"
        "bne %0, %2, 1b"
        : "=&r"(hi), "=&r"(lo), "=&r"(tmp));
    return ((uint64_t)hi << 32) | lo;
#endif
}

// Hardcode timebase
static uint64_t timebase = 100000;

/* *
 * clock_init - initialize 8253 clock to interrupt 100 times per second,
 * and then enable IRQ_TIMER.
 * */
void clock_init(void) {
    // enable timer interrupt in sie
    set_csr(sie, MIP_STIP);
    //set_csv是宏，定义在riscv.h，让CPU将指定CSR中对应的位设为1，其他位保持不变
    // sie 是 CPU 内部的 S 模式中断使能寄存器。
    // • MIP_STIP 是本代码使用的位掩码，位置对应 S 模式时钟中断。
    // • 把这个掩码用于 sie，就是设置其时钟中断使能位。
    // divided by 500 when using Spike(2MHz)
    // divided by 100 when using QEMU(10MHz)
    // timebase = sbi_timebase() / 500;
    clock_set_next_event();
//设置第一次时钟事件
    // initialize time counter 'ticks' to zero
    ticks = 0;
//将已经处理的时钟中断次数初始化为0
    cprintf("++ setup timer interrupts\n");
}

void clock_set_next_event(void) { sbi_set_timer(get_cycles() + timebase); }
//get_cycles()取得当前时间计数
//通过 OpenSBI 接口设置这个未来时间点。该接口声明在：libs/sbi.h
