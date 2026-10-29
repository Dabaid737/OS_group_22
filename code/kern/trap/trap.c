#include <assert.h>
#include <clock.h>
#include <console.h>
#include <defs.h>
#include <kdebug.h>
#include <memlayout.h>
#include <mmu.h>
#include <riscv.h>
#include <sbi.h>
#include <stdio.h>
#include <trap.h>

#define TICK_NUM 100
//我们每处理100次时钟中断，就输出一次统计信息
/* C 部分的实验约定：每 100 次时钟中断输出一次，输出 10 次后关机。
 * 原始框架未定义此参数；这里的单位是输出次数，不是中断次数。
 */
#ifndef SHUTDOWN_AFTER
#define SHUTDOWN_AFTER 10
#endif

static void print_ticks() {
    cprintf("%d ticks\n", TICK_NUM);
    //这是编译时条件：• 编译时定义了 DEBUG_GRADE，这部分才会参与编译。• 没有定义，它就不进入生成的程序。
#ifdef DEBUG_GRADE
//累计输出达到 SHUTDOWN_AFTER 后，需要调用关机接口
    cprintf("End of Test.\n");
    panic("EOT: kernel seems ok.");
#endif
}

/* idt_init - initialize IDT to each of the entry points in kern/trap/vectors.S
 */
void idt_init(void) { //提前设置Trap入口

    extern void __alltraps(void);//告诉编译器：__alltraps在其他地方定义,就是trapentry.S中的入口标签
    /* Set sup0 scratch register to 0, indicating to exception vector
       that we are presently executing in the kernel */
    write_csr(sscratch, 0);//将sscratch清零，按照这份代码的约定，表示当前正在内核中执行。
    /* Set the exception vector address */
    write_csr(stvec, &__alltraps); /*把trapentry.S中__alltraps的地址写入stvec寄存器，告诉CPU异常发生时跳转到__alltraps,然后SAVE_ALL */
}

/* trap_in_kernel - test if trap happened in kernel */
bool trap_in_kernel(struct trapframe *tf) { //判断被打断前是否处于内核态(1表示S，0表示U)
    return (tf->status & SSTATUS_SPP) != 0;
}

void print_trapframe(struct trapframe *tf) { //打印整份现场
    cprintf("trapframe at %p\n", tf);//tf：这份现场保存在内存的哪个地址。
    print_regs(&tf->gpr);//print_regs(&tf->gpr)：现场中的通用寄存器。
    cprintf("  status   0x%08x\n", tf->status);//后面四个字段：状态、返回地址、异常相关地址和原因。
    cprintf("  epc      0x%08x\n", tf->epc);
    cprintf("  badvaddr 0x%08x\n", tf->badvaddr);
    cprintf("  cause    0x%08x\n", tf->cause);
}

void print_regs(struct pushregs *gpr) { //打印保存的通用寄存器
    cprintf("  zero     0x%08x\n", gpr->zero);
    cprintf("  ra       0x%08x\n", gpr->ra);
    cprintf("  sp       0x%08x\n", gpr->sp);
    cprintf("  gp       0x%08x\n", gpr->gp);
    cprintf("  tp       0x%08x\n", gpr->tp);
    cprintf("  t0       0x%08x\n", gpr->t0);
    cprintf("  t1       0x%08x\n", gpr->t1);
    cprintf("  t2       0x%08x\n", gpr->t2);
    cprintf("  s0       0x%08x\n", gpr->s0);
    cprintf("  s1       0x%08x\n", gpr->s1);
    cprintf("  a0       0x%08x\n", gpr->a0);
    cprintf("  a1       0x%08x\n", gpr->a1);
    cprintf("  a2       0x%08x\n", gpr->a2);
    cprintf("  a3       0x%08x\n", gpr->a3);
    cprintf("  a4       0x%08x\n", gpr->a4);
    cprintf("  a5       0x%08x\n", gpr->a5);
    cprintf("  a6       0x%08x\n", gpr->a6);
    cprintf("  a7       0x%08x\n", gpr->a7);
    cprintf("  s2       0x%08x\n", gpr->s2);
    cprintf("  s3       0x%08x\n", gpr->s3);
    cprintf("  s4       0x%08x\n", gpr->s4);
    cprintf("  s5       0x%08x\n", gpr->s5);
    cprintf("  s6       0x%08x\n", gpr->s6);
    cprintf("  s7       0x%08x\n", gpr->s7);
    cprintf("  s8       0x%08x\n", gpr->s8);
    cprintf("  s9       0x%08x\n", gpr->s9);
    cprintf("  s10      0x%08x\n", gpr->s10);
    cprintf("  s11      0x%08x\n", gpr->s11);
    cprintf("  t3       0x%08x\n", gpr->t3);
    cprintf("  t4       0x%08x\n", gpr->t4);
    cprintf("  t5       0x%08x\n", gpr->t5);
    cprintf("  t6       0x%08x\n", gpr->t6);
}

//lab3 function2 2512621
/* C：中断分类与时钟处理。保留现场，返回后由入口汇编恢复执行。 */
void interrupt_handler(struct trapframe *tf) {
    /* 去掉最高位的中断标记，只用局部变量保存具体原因号。 */
    uintptr_t cause = tf->cause & ~((uintptr_t)1 << 63);

    switch (cause) {
    case IRQ_U_SOFT:
        cprintf("User software interrupt\n");
        break;
    case IRQ_S_SOFT:
        cprintf("Supervisor software interrupt\n");
        break;
    case IRQ_H_SOFT:
        cprintf("Hypervisor software interrupt\n");
        break;
    case IRQ_M_SOFT:
        cprintf("Machine software interrupt\n");
        break;
    case IRQ_U_TIMER:
        cprintf("User timer interrupt\n");
        break;
    case IRQ_S_TIMER:
        /* SBI 定时接口设置一次事件，每次处理中断后重新预约下一次。 */
        clock_set_next_event();
        ticks++;

        if (ticks % TICK_NUM == 0) {
            print_ticks();
            /* 第 200 次中断可以正常返回；默认到第 1000 次才关机。 */
            if (ticks / TICK_NUM >= SHUTDOWN_AFTER) {
                sbi_shutdown();
            }
        }
        break;
    case IRQ_H_TIMER:
        cprintf("Hypervisor timer interrupt\n");
        break;
    case IRQ_M_TIMER:
        cprintf("Machine timer interrupt\n");
        break;
    case IRQ_U_EXT:
        cprintf("User external interrupt\n");
        break;
    case IRQ_S_EXT:
        cprintf("Supervisor external interrupt\n");
        break;
    case IRQ_H_EXT:
        cprintf("Hypervisor external interrupt\n");
        break;
    case IRQ_M_EXT:
        cprintf("Machine external interrupt\n");
        break;
    default:
        cprintf("Unknown interrupt: cause %lu\n", (unsigned long)cause);
        print_trapframe(tf);
        break;
    }
}
//lab3 function2 2511461
/* 处理已经由 trap_dispatch 分流的同步异常。
 * 仅在确认可以跳过指令时修改 tf->epc，其余保存的现场保持不变。
 * 返回后由已有入口汇编恢复现场，将 tf->epc 写回 sepc 并执行 sret。
 */
void exception_handler(struct trapframe *tf) {
    switch (tf->cause) {
    case CAUSE_ILLEGAL_INSTRUCTION: {
        /* 在推进 epc 前打印原始异常地址；%llx 保留工程中 64 位地址的完整值。 */
        cprintf("Illegal instruction at 0x%llx\n",
                (unsigned long long)tf->epc);
        /* 按半字读取指令，兼容仅按 2 字节对齐的指令地址。
         * 依赖调用前提保证所读内存安全可读；这里不提供故障地址的安全取指机制。
         */
        const volatile uint16_t *insn = (const volatile uint16_t *)tf->epc;
        uint16_t first = insn[0];

        /* 低两位不是 11，编码长度为 16 位；本模块直接跳过，不模拟指令。 */
        if ((first & 0x3) != 0x3) {
            tf->epc += 2;
        /* 低两位为 11，且位 [4:2] 不是 111，编码长度为 32 位。 */
        } else if ((first & 0x1f) != 0x1f) {
            tf->epc += 4;
        } else {
            /* 低五位全为 1：超出本任务支持范围，不猜测长度，保留 epc。 */
            print_trapframe(tf);
        }
        return;
    }
    case CAUSE_BREAKPOINT: {
        /* 同样先输出修改前的断点地址，再检查实际的软件断点编码。 */
        cprintf("Breakpoint at 0x%llx\n", (unsigned long long)tf->epc);
        const volatile uint16_t *insn = (const volatile uint16_t *)tf->epc;
        uint16_t first = insn[0];

        /* 精确匹配压缩软件断点 C.EBREAK（0x9002），长度为 2 字节。 */
        if (first == MATCH_C_EBREAK) {
            tf->epc += 2;
        /* 普通 EBREAK 为 0x00100073，分别匹配低、高半字。
         * && 的短路求值保证只有低半字匹配后，才读取第二个半字。
         */
        } else if (first == (MATCH_EBREAK & 0xffff) &&
                   insn[1] == (MATCH_EBREAK >> 16)) {
            tf->epc += 4;
        } else {
            /* 无法确认软件断点或来自其他断点来源时，仅打印现场，不推进 epc。 */
            print_trapframe(tf);
        }
        return;
    }
    default:
        /* 其他同步异常仅提供诊断，不读取 epc 指向的内存，也不修改现场。
         * 未修复的故障条件可能在返回后再次触发异常。
         */
        print_trapframe(tf);
        return;
    }
}


static inline void trap_dispatch(struct trapframe *tf) { //判断是中断还是异常
    if ((intptr_t)tf->cause < 0) {//tf->cause 保存的是 scause，最高位1（intptr_t 是有符号整数类型。把 cause 当作有符号数看时，最高位为 1 就表现为负数，因此可以这样区分。）表示中断，0表示异常
        // interrupts
        interrupt_handler(tf);
    } else {
        // exceptions
        exception_handler(tf);
    }
}

/* *
 * trap - handles or dispatches an exception/interrupt. if and when trap()
 * returns,
 * the code in kern/trap/trapentry.S restores the old CPU state saved in the
 * trapframe and then uses the iret instruction to return from the exception.
 * */
void trap(struct trapframe *tf) { //接收汇编传来的现场（jal trap）
    // dispatch based on what type of trap occurred
    trap_dispatch(tf);
}
