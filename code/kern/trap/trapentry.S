#include <riscv.h>

    .macro SAVE_ALL #负责把程序当前的状态保存下来

    csrw sscratch, sp #暂存原来的栈指针sp

    addi sp, sp, -36 * REGBYTES #在栈上腾出空间sp=sp-288(36*8)，向低地址增长
    # save x registers #把通用寄存器（x0-x30）存进对应的位置
    STORE x0, 0*REGBYTES(sp)
    STORE x1, 1*REGBYTES(sp) #x1是返回地址寄存器ra，存到sp+8（每个寄存器8字节）
    STORE x3, 3*REGBYTES(sp) #x2跳过原因：x2叫sp，是RISC-V的调用约定规定的别名（在trap.h中）,已经减288，所以后期另存
    STORE x4, 4*REGBYTES(sp)
    STORE x5, 5*REGBYTES(sp)
    STORE x6, 6*REGBYTES(sp)
    STORE x7, 7*REGBYTES(sp)
    STORE x8, 8*REGBYTES(sp)  #s0
    STORE x9, 9*REGBYTES(sp)  #s1原来的数据，但不一定和CPU状态有关，不要和后面混了
    STORE x10, 10*REGBYTES(sp) 
    STORE x11, 11*REGBYTES(sp)
    STORE x12, 12*REGBYTES(sp)
    STORE x13, 13*REGBYTES(sp)
    STORE x14, 14*REGBYTES(sp)
    STORE x15, 15*REGBYTES(sp)
    STORE x16, 16*REGBYTES(sp)
    STORE x17, 17*REGBYTES(sp)
    STORE x18, 18*REGBYTES(sp) #s2
    STORE x19, 19*REGBYTES(sp) #s3
    STORE x20, 20*REGBYTES(sp) #s4
    STORE x21, 21*REGBYTES(sp)
    STORE x22, 22*REGBYTES(sp)
    STORE x23, 23*REGBYTES(sp)
    STORE x24, 24*REGBYTES(sp)
    STORE x25, 25*REGBYTES(sp)
    STORE x26, 26*REGBYTES(sp)
    STORE x27, 27*REGBYTES(sp)
    STORE x28, 28*REGBYTES(sp)
    STORE x29, 29*REGBYTES(sp)
    STORE x30, 30*REGBYTES(sp)
    STORE x31, 31*REGBYTES(sp)

    # get sr, epc, badvaddr, cause
    # Set sscratch register to 0, so that if a recursive exception
    # occurs, the exception vector knows it came from the kernel
    csrrw s0, sscratch, x0 #把sscratch中保存的原sp读到s0,把sscratch清零  接下来读几个CSR(CSR是“控制与状态寄存器”，用来记录CPU状态、控制CPU的某些行为)
    csrr s1, sstatus #保存与返回有关的处理器状态
    csrr s2, sepc #保存返回后执行的位置
    csrr s3, sbadaddr #记录与某些异常有关的地址信息
    csrr s4, scause #说明发生了哪种异常或中断
    #把这些值也存进栈
    STORE s0, 2*REGBYTES(sp) #这里把原来的sp补到x2对应的槽位
    STORE s1, 32*REGBYTES(sp)
    STORE s2, 33*REGBYTES(sp)
    STORE s3, 34*REGBYTES(sp)
    STORE s4, 35*REGBYTES(sp)
    .endm  #当前sp指向的就是完整的trapframe

    .macro RESTORE_ALL #宏，把栈里保存的现场恢复回寄存器

    #lab3 function1 2512752

    # 恢复状态和返回地址
    LOAD s0, 32*REGBYTES(sp) #取出刚进入这次中断或异常时的sstatus值（包括1.进入Trap后的当前状态：例如中断已被关闭2.CPU记录的之前状态）
    LOAD s1, 33*REGBYTES(sp) #取出栈里保存sepc值（准备返回执行的指令地址）
    csrw sstatus, s0 #写回sstatus
    csrw sepc, s1 #写回sepc，最后由sret跳过去     #这里s0,s1都还没有恢复到原来的值

    #从栈里的对应位置，取回这个寄存器原来的值
    LOAD x1, 1*REGBYTES(sp) #跳过x0因为恒为0不需要恢复
    LOAD x3, 3*REGBYTES(sp)
    LOAD x4, 4*REGBYTES(sp)
    LOAD x5, 5*REGBYTES(sp)
    LOAD x6, 6*REGBYTES(sp)
    LOAD x7, 7*REGBYTES(sp)
    LOAD x8, 8*REGBYTES(sp) #s0
    LOAD x9, 9*REGBYTES(sp) #s1
    LOAD x10, 10*REGBYTES(sp)
    LOAD x11, 11*REGBYTES(sp)
    LOAD x12, 12*REGBYTES(sp)
    LOAD x13, 13*REGBYTES(sp)
    LOAD x14, 14*REGBYTES(sp)
    LOAD x15, 15*REGBYTES(sp)
    LOAD x16, 16*REGBYTES(sp)
    LOAD x17, 17*REGBYTES(sp)
    LOAD x18, 18*REGBYTES(sp)
    LOAD x19, 19*REGBYTES(sp)
    LOAD x20, 20*REGBYTES(sp)
    LOAD x21, 21*REGBYTES(sp)
    LOAD x22, 22*REGBYTES(sp)
    LOAD x23, 23*REGBYTES(sp)
    LOAD x24, 24*REGBYTES(sp)
    LOAD x25, 25*REGBYTES(sp)
    LOAD x26, 26*REGBYTES(sp)
    LOAD x27, 27*REGBYTES(sp)
    LOAD x28, 28*REGBYTES(sp)
    LOAD x29, 29*REGBYTES(sp)
    LOAD x30, 30*REGBYTES(sp)
    LOAD x31, 31*REGBYTES(sp)

    #从保存的现场中取回原来的栈指针
    LOAD x2, 2*REGBYTES(sp)

    .endm

    .globl __alltraps #让其他文件能够引用__alltraps这个符号
    .align(2) #让入口地址按 2^2=4字节对齐，满足这里Trap入口的要求  #入口按4字节对齐；trap.c中的idt_init()将此入口地址写入stvec
__alltraps:
    SAVE_ALL

    #lab3 function1 2512752

    # 将trapframe地址作为第一个参数，调用C函数
    mv a0, sp
    jal trap #1.把后面指令的地址写进ra,记住处理完回到哪里2.跳到trap()执行 处理完之后通过函数返回指令ret，按照返回地址回到__trapret恢复现场
    # sp should be the same as before "jal trap"

    .globl __trapret
__trapret: #C函数返回，接着恢复现场
    RESTORE_ALL
    # return from supervisor call
    sret #返回原程序
