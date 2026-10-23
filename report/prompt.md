# Buddy System 任务提示词记录

以下按任务阶段保存本次实现、补充测试及调整测试要求的任务提示词；省略 IDE 标签和打开文件列表。报告正文仅引用本文件。

## 一、实现与接入

[PROMPT]

**任务**：在 kern/mm/ 目录下新建 buddy_system.c 和 buddy_system.h，基于现有物理内存管理框架，实现并接入 Buddy System（伙伴系统）连续物理页分配模块。
模块接收 page_init() 交付的可用页面，以每阶一个空闲链表的方式管理内存，完成空闲区登记、页面分配、块拆分、页面释放、伙伴合并及空闲页统计。

**操作要求**：必须在实际工程中创建文件并完成模块实现，而不是仅展示代码。可以根据需要新增模块内部的辅助函数、宏定义和静态变量；辅助函数的使用范围应尽量限定在模块内部。以工程已有接口和数据结构为准，保留与本任务无关的现有代码。

**输出要求**：完成文件操作后，简要说明新增文件及其职责、采用的数据结构和主要实现思路；如修改了现有文件，应说明修改位置、目的及其效果。同时报告实际执行的验证及结果；对于尚未验证或受其他模块依赖影响的部分，应如实说明；无修改无验证则无需说明。

[RELY]

### 1. 页、页描述符、页状态及空闲页管理结构

#### 1.1 页相关宏定义

kern/mm/mmu.h 中定义：

```c
#define PGSIZE 4096 // 页大小：4096字节
#define PGSHIFT 12  // log2(PGSIZE)，也是页内偏移的位数
```

#### 1.2 页描述符定义

kern/mm/memlayout.h 中定义：

```c
struct Page {
    int ref;                  // 引用计数
    uint64_t flags;           // 64位无符号整数，保存页面状态
    unsigned int property;   // 现有First-Fit、Best-Fit用于记录空闲块页数
    list_entry_t page_link;   // 嵌入页描述符中的双向链表节点
};
```

空闲块通过块首页描述符中的 page_link 加入链表，不需要将块内每一页都挂入链表。
pmm.h 提供 page_ref() 和 set_page_ref()，用于读取和设置引用计数。

#### 1.3 页状态标志位及操作

```c
#define PG_reserved 0 // flags第0位：页面是否被保留
#define PG_property 1 // flags第1位：是否为空闲块首页
#define SetPageReserved(page) ((page)->flags |= (1UL << PG_reserved))
#define ClearPageReserved(page) ((page)->flags &= ~(1UL << PG_reserved))
#define PageReserved(page) (((page)->flags >> PG_reserved) & 1)
#define SetPageProperty(page) ((page)->flags |= (1UL << PG_property))
#define ClearPageProperty(page) ((page)->flags &= ~(1UL << PG_property))
#define PageProperty(page) (((page)->flags >> PG_property) & 1)
```

Set、Clear 及读取操作分别用于将对应位置 1、置 0 和读取该位。
PG_reserved 为 1，表示该页被保留，不能作为普通空闲页分配；为 0，只表示没有保留标记，不代表该页一定空闲。
PG_property 为 1，表示该页是空闲块首页；为 0，可能表示页面已分配，也可能表示它是块内部的页面。

#### 1.4 已有的空闲页管理结构

```c
typedef struct {
    list_entry_t free_list; // 空闲块链表的表头
    unsigned int nr_free;  // 链表所管理的空闲页总数
} free_area_t;
```

### 2. 页描述符与物理地址的对应关系

pmm.h 中声明，并在 pmm.c 中定义：

```c
extern struct Page *pages; // 指向页描述符数组的起点
extern size_t npage;       // 可管理物理页号的排他上界
extern const size_t nbase; // 第一个描述符对应的物理页号
```

描述符总数为 npage - nbase。pages[i] 对应物理页号 nbase + i。
描述符数组中包含保留页的记录，不能将全部描述符都视为空闲页。

pmm.h 中提供以下转换函数：

```c
static inline ppn_t page2ppn(struct Page *page) {
    return page - pages + nbase;
}
static inline uintptr_t page2pa(struct Page *page) {
    return page2ppn(page) << PGSHIFT;
}
```

page2ppn() 将描述符指针转换为物理页号。page2pa() 将描述符指针转换为页面起始物理地址。
page - pages 得到的是描述符数组中的元素下标，不是字节差。
这两个函数要求传入合法的描述符指针，本身不进行范围检查。

### 3. 链表结构及工具

libs/list.h 中定义：

```c
struct list_entry {
    struct list_entry *prev, *next;
};
typedef struct list_entry list_entry_t;
```

list_entry_t 是双向链表节点类型，包含前驱和后继指针。
工程提供双向循环链表，表头本身不代表某个物理页。可使用的操作包括：

- list_init：初始化链表头或节点。
- list_empty：判断链表是否为空。
- list_next、list_prev：取得后继、前驱节点。
- list_add_before、list_add_after：在指定节点之前、之后插入节点。
- list_del：摘除节点，但不重新初始化节点。
- list_del_init：摘除节点并重新初始化。

插入函数的第一个参数是已有节点，第二个参数是待插入节点。
kern/mm/memlayout.h 还提供：

```c
#define le2page(le, member) to_struct((le), struct Page, member)
```

le2page(le, page_link) 用于从链表节点取得它所在的 Page 描述符。
遍历时必须先判断是否到达表头，不能将表头转换为 Page 描述符。

[GUARANTEE]

在 buddy_system.c 中实现以下函数：

```c
static void buddy_init(void); // 初始化各阶链表和统计信息
static void buddy_init_memmap(struct Page *base, size_t n); // 将可用页面组织为伙伴块
static struct Page *buddy_alloc_pages(size_t n); // 分配满足请求的连续页面
static void buddy_free_pages(struct Page *base, size_t n); // 释放完整块并尝试合并伙伴
static size_t buddy_nr_free_pages(void); // 返回空闲页总数
static void buddy_check(void); // 检查伙伴分配器的正确性
```

定义 const struct pmm_manager buddy_pmm_manager，将上述函数注册到对应接口，并在 buddy_system.h 中声明该对象。
修改 pmm.c 中的 init_pmm_manager()，使其选择并初始化 buddy_pmm_manager，保留其他分配器代码。
允许根据需要添加模块内部的静态辅助函数、宏和变量。

[specification]

1. 通过物理内存管理器的 init_memmap() 接口，接收 page_init() 提供的可用物理页区间。
2. 将可用区间划分为若干合法空闲块。每个块的页数必须为 2 的整数次幂，起始位置满足相应的对齐要求，且不得超出可用区间。每次选取当前起点处能够容纳的最大合法块，继续处理剩余部分，直至全部可用页面登记完毕。最小分配单位为一页，即 4096 字节。
3. 当请求页数恰好为 2^m 时，优先从对应阶的空闲链表中取得一个块；若该阶没有空闲块，则向更高阶查找，并逐级对半拆分，直到得到目标阶的块。拆分产生的剩余块应登记到对应阶的空闲链表中；若不存在足够大的空闲块，则分配失败。
4. 当请求页数 n 不是 2 的整数次幂时，将其向上取整为满足 2^u ≥ n 的最小块大小，再按照上述规则进行分配。
5. 释放时，以此前完整分配的块为单位进行回收，不支持部分释放。若其伙伴块空闲、阶数相同，且合并不会越过管理边界，则将两者合并，并继续尝试向更高阶合并。

### 实现阶段的补充要求

暂时无需进行校验，先生成文件即可。

## 二、补充测试

[PROMPT]

任务：阅读当前 kern/mm/buddy_system.c、buddy_system.h 及相关内存管理代码，为已有 Buddy 分配器补充测试。

新增 buddy_test_all()，从 buddy_check() 中调用。
测试及辅助函数尽量放在 buddy_system.c 内部，并使用 static，保留已有检查。

操作要求：必须在实际工程中添加测试代码。保留现有分配算法，发现错误时报告失败位置，不通过修改算法、删除断言或降低测试要求来获得通过。

输出要求：简要说明测试覆盖范围、运行方法及实际验证结果。未执行的测试不得声称通过。

[SPECIFICATION]

### 1. 初始化与边界

在隔离环境中测试非 2 的幂长度、起点不满足高阶对齐，以及带保留间隔的可用区域。

检查所有可用页面均被登记，块大小和对齐正确，不会越界，也不会将保留页纳入分配。

不得破坏真实管理器；临时修改的描述符、全局变量和链表状态，测试结束后必须完整恢复。

### 2. 基本分配与向上取整

测试申请 1、2、4、8 页，以及 3、5、9 页。
在资源足够时必须成功，后者实际占用应分别为 4、8、16 页。

检查返回块的范围、对齐、状态及空闲页变化。
释放时传入原始请求页数，确认整个实际块被归还。
期望结果应独立确定，不直接复用被测算法计算。

### 3. 拆分与伙伴合并

从已知大块申请小块，检查逐级拆分后剩余块登记正确。
按不同顺序释放子块，确认伙伴能够逐级合并，最终可以重新申请完整大块。

伙伴尚未释放时不能合并；相邻且同阶、但不是伙伴的块，以及被保留区域隔开的块，也不能错误合并。

### 4. 分配失败

在可控环境中测试内存耗尽、请求超过支持范围，以及总空闲页足够但没有足够大连续伙伴块的情况。

失败应返回 NULL，原有链表和统计信息不变。
当前申请 0 页会触发断言，不放入默认测试流程；如需验证，应独立运行。

### 5. 分配不重叠与数据保持

同时保留多个分配块，检查它们的实际占用区间互不重叠。
向真实可访问的分配页面写入不同数据，再读取核对，确认没有重复分配或相互覆盖。

不得向模拟描述符对应的虚构物理地址写入数据。

### 6. 链表与统计一致性

检查各阶链表按物理地址升序排列，前后指针一致，节点无重复，空闲块大小、对齐和首页标记正确。

各空闲块不能重叠，页数之和应等于空闲页总数。
拆分、合并不改变总数，分配和释放按实际块页数更新。
遍历应设置合理上限，避免链表损坏后永久卡住。

### 7. 测试结束与运行结果

每组测试结束后归还所有测试页面，最终恢复空闲页总数，并确认管理器仍能正常分配。

启动栈只有 8 KiB，大型测试数组使用静态存储。

测试开始打印“[BuddyTest] BEGIN”，每项成功后打印名称和 PASS。
所有必测项目通过后，才打印“[BuddyTest] ALL PASS”。
失败时报告测试名称和相关信息，并触发断言；不得跳过必测项目后仍宣称全部通过。

## 三、测试要求的后续修订

修改检验，无需保证地址升序要求，保留原有的尾插方法。

该修订替代第二阶段第 6 项中的地址升序要求，其他测试要求保留。
