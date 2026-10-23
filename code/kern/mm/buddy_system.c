#include <buddy_system.h>
#include <list.h>
#include <stdio.h>
#include <string.h>

/* property must be able to represent the size of every supported block. */
#define BUDDY_MAX_ORDER (sizeof(unsigned int) * 8 - 1)

static free_area_t buddy_areas[BUDDY_MAX_ORDER + 1];
static size_t buddy_nr_free;

static size_t
buddy_block_size(unsigned int order) {
    return (size_t)1 << order;
}

/* Reject oversized requests before doubling, avoiding rounding overflow. */
static bool
buddy_request_order(size_t n, unsigned int *order) {
    if (n == 0 || n > buddy_block_size(BUDDY_MAX_ORDER)) {
        return 0;
    }
    *order = 0;
    while (buddy_block_size(*order) < n) {
        (*order)++;
    }
    return 1;
}

static void
buddy_add_block(struct Page *base, unsigned int order) {
    size_t size = buddy_block_size(order);
    base->property = size;
    SetPageProperty(base);
    list_add_before(&buddy_areas[order].free_list, &base->page_link);
    buddy_areas[order].nr_free += size;
}

static void
buddy_remove_block(struct Page *base, unsigned int order) {
    list_del_init(&base->page_link);
    ClearPageProperty(base);
    base->property = 0;
    buddy_areas[order].nr_free -= buddy_block_size(order);
}

/* Check physical bounds before converting the buddy PPN to a descriptor. */
static struct Page *
buddy_find_free_partner(struct Page *base, unsigned int order) {
    size_t size = buddy_block_size(order);
    size_t partner_ppn = page2ppn(base) ^ size;
    if (partner_ppn < nbase || partner_ppn >= npage ||
        size > npage - partner_ppn) {
        return NULL;
    }
    struct Page *partner = pages + (partner_ppn - nbase);
    if (PageReserved(partner) || !PageProperty(partner) ||
        partner->property != size) {
        return NULL;
    }
    return partner;
}

/* Only registered free heads can merge, so reserved holes stay excluded. */
static void
buddy_release_block(struct Page *base, unsigned int order) {
    while (order < BUDDY_MAX_ORDER) {
        struct Page *partner = buddy_find_free_partner(base, order);
        if (partner == NULL) {
            break;
        }
        buddy_remove_block(partner, order);
        if (partner < base) {
            base = partner;
        }
        order++;
    }
    buddy_add_block(base, order);
}

static void
buddy_init(void) {
    for (unsigned int order = 0; order <= BUDDY_MAX_ORDER; order++) {
        list_init(&buddy_areas[order].free_list);
        buddy_areas[order].nr_free = 0;
    }
    buddy_nr_free = 0;
}

static void
buddy_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);
    assert(base >= pages && base < pages + (npage - nbase));
    assert(n <= (size_t)(pages + (npage - nbase) - base));
    for (struct Page *p = base; p != base + n; p++) {
        assert(PageReserved(p));
    }
    for (struct Page *p = base; p != base + n; p++) {
        p->flags = 0;
        p->property = 0;
        set_page_ref(p, 0);
        list_init(&p->page_link);
    }

    buddy_nr_free += n;
    while (n != 0) {
        unsigned int order = 0;
        size_t ppn = page2ppn(base);
        /* Greedily choose the largest physically aligned block that fits. */
        while (order < BUDDY_MAX_ORDER) {
            size_t next_size = buddy_block_size(order + 1);
            if (next_size > n || (ppn & (next_size - 1)) != 0) {
                break;
            }
            order++;
        }
        size_t size = buddy_block_size(order);
        buddy_release_block(base, order);
        base += size;
        n -= size;
    }
}

static struct Page *
buddy_alloc_pages(size_t n) {
    assert(n > 0);
    unsigned int target;
    if (!buddy_request_order(n, &target) ||
        buddy_block_size(target) > buddy_nr_free) {
        return NULL;
    }
    unsigned int order = target;
    while (order <= BUDDY_MAX_ORDER &&
           list_empty(&buddy_areas[order].free_list)) {
        order++;
    }
    if (order > BUDDY_MAX_ORDER) {
        return NULL;
    }

    struct Page *base = le2page(
        list_next(&buddy_areas[order].free_list), page_link);
    buddy_remove_block(base, order);
    while (order > target) {
        order--;
        buddy_add_block(base + buddy_block_size(order), order);
    }
    /* A non-free head retains its allocated size to reject partial frees.
     * All interior descriptors have property == 0. PG_property alone marks
     * free heads; a nonzero property alone never implies that a page is free.
     */
    base->property = buddy_block_size(target);
    buddy_nr_free -= buddy_block_size(target);
    return base;
}

static void
buddy_free_pages(struct Page *base, size_t n) {
    unsigned int order;
    assert(buddy_request_order(n, &order));
    size_t size = buddy_block_size(order);
    assert(base >= pages && base < pages + (npage - nbase));
    assert(size <= (size_t)(pages + (npage - nbase) - base));
    assert((page2ppn(base) & (size - 1)) == 0);
    assert(!PageReserved(base) && !PageProperty(base));
    assert(base->property == size);
    for (struct Page *p = base; p != base + size; p++) {
        assert(!PageReserved(p) && !PageProperty(p));
        assert(p == base || p->property == 0);
    }
    for (struct Page *p = base; p != base + size; p++) {
        p->flags = 0;
        p->property = 0;
        set_page_ref(p, 0);
    }
    buddy_nr_free += size;
    buddy_release_block(base, order);
}

static size_t
buddy_nr_free_pages(void) {
    return buddy_nr_free;
}

/* Test storage is static: the boot stack is only 8 KiB. No test changes
 * nbase (which is const), the selected manager, or the address mapping.
 */
#define BT_FAKE_PAGES 160
#define BT_MAX_PAGES ((KMEMSIZE + KERNEL_BEGIN_PADDR - DRAM_BASE) / PGSIZE)
static struct Page bt_fake[BT_FAKE_PAGES];
static free_area_t bt_saved_areas[BUDDY_MAX_ORDER + 1];
static free_area_t bt_failure_areas[BUDDY_MAX_ORDER + 1];
static struct Page bt_failure_pages[BT_FAKE_PAGES];
static unsigned char bt_seen[BT_MAX_PAGES];
static unsigned char bt_available[BT_FAKE_PAGES];
static unsigned char bt_taken[BT_FAKE_PAGES];
static struct Page *bt_blocks[7];
static const size_t bt_requests[] = {1, 2, 4, 8, 3, 5, 9};
/* Independent expected results, not buddy_request_order/block_size. */
static const size_t bt_sizes[] = {1, 2, 4, 8, 4, 8, 16};
static const char *bt_name;
static size_t bt_origin, bt_detail_a, bt_detail_b;
static bool bt_isolated;

static bool
bt_expect(bool ok, const char *condition, int line) {
    if (!ok) {
        cprintf("[BuddyTest] %s FAIL line=%d: %s (a=%lu b=%lu)\n",
                bt_name, line, condition, (unsigned long)bt_detail_a,
                (unsigned long)bt_detail_b);
    }
    return ok;
}

#define BT_REQUIRE(condition) do { \
    if (!bt_expect((condition), #condition, __LINE__)) return 0; \
} while (0)

/* Validate an entry address before dereferencing it or using le2page. */
static bool
bt_node_valid(list_entry_t *le, list_entry_t *head) {
    uintptr_t address = (uintptr_t)le;
    uintptr_t first = (uintptr_t)&pages[0].page_link;
    return le == head || (address >= first &&
        address - first < (npage - nbase) * sizeof(struct Page) &&
        (address - first) % sizeof(struct Page) == 0);
}

static bool
bt_lists(void) {
    size_t count = npage - nbase, total = 0;
    BT_REQUIRE(count <= BT_MAX_PAGES);
    memset(bt_seen, 0, count);
    for (unsigned int order = 0; order <= BUDDY_MAX_ORDER; order++) {
        list_entry_t *head = &buddy_areas[order].free_list;
        list_entry_t *previous = head;
        size_t subtotal = 0, visited = 0;
        size_t size = (size_t)1 << order;
        BT_REQUIRE(bt_node_valid(head->next, head));
        BT_REQUIRE(bt_node_valid(head->prev, head));
        for (list_entry_t *le = head->next; le != head; le = le->next) {
            bt_detail_a = order;
            bt_detail_b = visited;
            BT_REQUIRE(++visited <= count);
            BT_REQUIRE(bt_node_valid(le, head));
            BT_REQUIRE(bt_node_valid(le->next, head));
            BT_REQUIRE(le->prev == previous && le->next->prev == le);
            struct Page *p = le2page(le, page_link);
            size_t index = p - pages, ppn = nbase + index;
            bt_detail_a = ppn;
            bt_detail_b = order;
            /* Tail insertion is valid; verify coverage independently of order. */
            BT_REQUIRE(size <= count - index && ppn % size == 0);
            BT_REQUIRE(PageProperty(p) && p->property == size);
            for (size_t i = 0; i < size; i++) {
                bt_detail_a = index + i;
                BT_REQUIRE(!bt_seen[index + i]);
                bt_seen[index + i] = 1;
                BT_REQUIRE(!PageReserved(p + i) && page_ref(p + i) == 0);
                BT_REQUIRE(i == 0 || (!PageProperty(p + i) &&
                                      p[i].property == 0));
                if (bt_isolated) {
                    BT_REQUIRE(bt_available[index + i] &&
                               !bt_taken[index + i]);
                }
            }
            subtotal += size;
            BT_REQUIRE(subtotal <= count);
            previous = le;
        }
        BT_REQUIRE(head->prev == previous && previous->next == head);
        BT_REQUIRE(subtotal == buddy_areas[order].nr_free);
        total += subtotal;
    }
    BT_REQUIRE(total == buddy_nr_free_pages());
    if (bt_isolated) {
        for (size_t i = 0; i < count; i++) {
            bt_detail_a = i;
            BT_REQUIRE(bt_seen[i] == (bt_available[i] && !bt_taken[i]));
            if (!bt_available[i]) {
                BT_REQUIRE(PageReserved(pages + i));
                BT_REQUIRE(!PageProperty(pages + i));
                BT_REQUIRE(pages[i].property == 0 && page_ref(pages + i) == 0);
                BT_REQUIRE(list_empty(&pages[i].page_link));
            }
        }
    }
    return 1;
}

static void
bt_reset(void) {
    buddy_init();
    memset(bt_fake, 0, sizeof(bt_fake));
    memset(bt_available, 0, sizeof(bt_available));
    memset(bt_taken, 0, sizeof(bt_taken));
    for (size_t i = 0; i < BT_FAKE_PAGES; i++) {
        SetPageReserved(bt_fake + i);
        list_init(&bt_fake[i].page_link);
    }
}

static void
bt_register(size_t first, size_t size) {
    for (size_t i = first; i < first + size; i++) {
        bt_available[i] = 1;
    }
    buddy_init_memmap(pages + first, size);
}

static bool
bt_allocated(struct Page *p, size_t size) {
    BT_REQUIRE(p != NULL);
    uintptr_t offset = (uintptr_t)p - (uintptr_t)pages;
    BT_REQUIRE((uintptr_t)p >= (uintptr_t)pages);
    BT_REQUIRE(offset % sizeof(struct Page) == 0);
    size_t index = offset / sizeof(struct Page);
    BT_REQUIRE(index < npage - nbase && size <= npage - nbase - index);
    BT_REQUIRE((nbase + index) % size == 0 && p->property == size);
    for (size_t i = 0; i < size; i++) {
        bt_detail_a = index + i;
        BT_REQUIRE(!PageReserved(p + i) && !PageProperty(p + i));
        BT_REQUIRE(page_ref(p + i) == 0);
        BT_REQUIRE(i == 0 || p[i].property == 0);
        if (bt_isolated) {
            BT_REQUIRE(bt_available[index + i] && !bt_taken[index + i]);
        }
    }
    if (bt_isolated) {
        memset(bt_taken + index, 1, size);
    }
    return 1;
}

static void
bt_return(struct Page *p, size_t request, size_t actual) {
    buddy_free_pages(p, request);
    if (bt_isolated) {
        memset(bt_taken + (p - pages), 0, actual);
    }
}

static bool
bt_initialization(void) {
    bt_reset();
    /* Non-power lengths, misaligned starts, and a reserved hole [15,33). */
    bt_register(bt_origin + 1, 14);
    bt_register(bt_origin + 33, 21);
    BT_REQUIRE(buddy_nr_free_pages() == 35);
    BT_REQUIRE(bt_lists());
    /* Exact greedy decomposition, independently listed by offset and size. */
    static const unsigned int starts[] = {1, 2, 4, 8, 12, 14,
                                         33, 34, 36, 40, 48, 52};
    static const unsigned int sizes[] = {1, 2, 4, 4, 2, 1,
                                        1, 2, 4, 8, 4, 2};
    for (size_t i = 0; i < sizeof(starts) / sizeof(starts[0]); i++) {
        struct Page *p = pages + bt_origin + starts[i];
        BT_REQUIRE(PageProperty(p) && p->property == sizes[i]);
    }
    for (size_t i = 0; i < 35; i++) {
        struct Page *p = buddy_alloc_pages(1);
        BT_REQUIRE(bt_allocated(p, 1));
        BT_REQUIRE(buddy_nr_free_pages() == 34 - i);
        BT_REQUIRE(bt_lists());
    }
    BT_REQUIRE(buddy_alloc_pages(1) == NULL);
    for (size_t i = 0; i < BT_FAKE_PAGES; i++) {
        if (bt_taken[i]) {
            bt_return(pages + i, 1, 1);
        }
    }
    BT_REQUIRE(buddy_nr_free_pages() == 35);
    return bt_lists();
}

static bool
bt_basic(void) {
    bt_reset();
    bt_register(bt_origin, 64);
    for (size_t i = 0; i < 7; i++) {
        size_t before = buddy_nr_free_pages();
        bt_blocks[i] = buddy_alloc_pages(bt_requests[i]);
        BT_REQUIRE(bt_allocated(bt_blocks[i], bt_sizes[i]));
        BT_REQUIRE(before - buddy_nr_free_pages() == bt_sizes[i]);
        BT_REQUIRE(bt_lists());
    }
    for (size_t i = 7; i != 0; i--) {
        size_t before = buddy_nr_free_pages();
        bt_return(bt_blocks[i - 1], bt_requests[i - 1], bt_sizes[i - 1]);
        BT_REQUIRE(buddy_nr_free_pages() - before == bt_sizes[i - 1]);
        BT_REQUIRE(bt_lists());
    }
    BT_REQUIRE(buddy_nr_free_pages() == 64);
    return 1;
}

/* Snapshot every descriptor and every head, including pointer identities. */
static bool
bt_failure(size_t request) {
    memcpy(bt_failure_areas, buddy_areas, sizeof(buddy_areas));
    memcpy(bt_failure_pages, pages, sizeof(bt_failure_pages));
    size_t before = buddy_nr_free_pages();
    bt_detail_a = request;
    BT_REQUIRE(buddy_alloc_pages(request) == NULL);
    BT_REQUIRE(before == buddy_nr_free_pages());
    BT_REQUIRE(memcmp(bt_failure_areas, buddy_areas, sizeof(buddy_areas)) == 0);
    BT_REQUIRE(memcmp(bt_failure_pages, pages, sizeof(bt_failure_pages)) == 0);
    return bt_lists();
}

static bool
bt_failures(void) {
    bt_reset();
    bt_register(bt_origin, 8);
    BT_REQUIRE(bt_failure(((size_t)1 << BUDDY_MAX_ORDER) + 1));
    BT_REQUIRE(bt_failure((size_t)-1));
    for (size_t i = 0; i < 4; i++) {
        bt_blocks[i] = buddy_alloc_pages(2);
        BT_REQUIRE(bt_allocated(bt_blocks[i], 2));
    }
    BT_REQUIRE(buddy_nr_free_pages() == 0);
    BT_REQUIRE(bt_failure(1));
    /* Four free pages, separated by live allocated blocks: no order-2 block. */
    bt_return(bt_blocks[0], 2, 2);
    bt_return(bt_blocks[2], 2, 2);
    BT_REQUIRE(buddy_nr_free_pages() == 4);
    BT_REQUIRE(bt_failure(3));
    bt_return(bt_blocks[1], 2, 2);
    bt_return(bt_blocks[3], 2, 2);
    BT_REQUIRE(buddy_nr_free_pages() == 8);
    return bt_lists();
}

static bool
bt_split_merge(void) {
    bt_reset();
    bt_register(bt_origin, 16);
    struct Page *p = buddy_alloc_pages(1);
    BT_REQUIRE(bt_allocated(p, 1));
    BT_REQUIRE(p == pages + bt_origin && buddy_nr_free_pages() == 15);
    /* Splitting 16 -> 8 -> 4 -> 2 -> 1 leaves these exact right halves. */
    for (unsigned int order = 0; order < 4; order++) {
        size_t size = (size_t)1 << order;
        list_entry_t *head = &buddy_areas[order].free_list;
        BT_REQUIRE(head->next == &(p + size)->page_link);
        BT_REQUIRE(head->prev == head->next && head->next->next == head);
        BT_REQUIRE(buddy_areas[order].nr_free == size);
    }
    BT_REQUIRE(bt_lists());
    bt_return(p, 1, 1);
    BT_REQUIRE(buddy_nr_free_pages() == 16 && bt_lists());
    /* Merge four subblocks with both low-first and high-first orders. */
    static const unsigned int sequences[2][4] = {{0, 2, 1, 3}, {3, 1, 2, 0}};
    for (size_t pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < 4; i++) {
            bt_blocks[i] = buddy_alloc_pages(4);
            BT_REQUIRE(bt_allocated(bt_blocks[i], 4));
            BT_REQUIRE(bt_blocks[i] == pages + bt_origin + i * 4);
        }
        for (size_t i = 0; i < 4; i++) {
            unsigned int which = sequences[pass][i];
            bt_detail_a = pass;
            bt_detail_b = i;
            bt_return(bt_blocks[which], 4, 4);
            BT_REQUIRE(buddy_nr_free_pages() == (i + 1) * 4);
            if (i == 0) {
                BT_REQUIRE(bt_blocks[which]->property == 4);
            }
            BT_REQUIRE(bt_lists());
        }
        p = buddy_alloc_pages(16);
        BT_REQUIRE(bt_allocated(p, 16));
        BT_REQUIRE(p == pages + bt_origin && buddy_nr_free_pages() == 0);
        bt_return(p, 16, 16);
        BT_REQUIRE(bt_lists());
    }
    return 1;
}

static bool
bt_nonpartners(void) {
    bt_reset();
    /* Adjacent [4,8) and [8,12) are aligned order-2 blocks, not buddies. */
    bt_register(bt_origin + 4, 8);
    BT_REQUIRE(buddy_nr_free_pages() == 8 && bt_lists());
    BT_REQUIRE(bt_failure(8));
    struct Page *a = buddy_alloc_pages(4), *b = buddy_alloc_pages(4);
    BT_REQUIRE(bt_allocated(a, 4) && bt_allocated(b, 4));
    bt_return(a, 4, 4);
    bt_return(b, 4, 4);
    BT_REQUIRE(bt_lists());
    BT_REQUIRE(bt_failure(8));
    bt_reset();
    bt_register(bt_origin, 4);
    bt_register(bt_origin + 8, 4);
    BT_REQUIRE(buddy_nr_free_pages() == 8 && bt_lists());
    BT_REQUIRE(bt_failure(8));
    a = buddy_alloc_pages(4);
    b = buddy_alloc_pages(4);
    BT_REQUIRE(bt_allocated(a, 4) && bt_allocated(b, 4));
    bt_return(a, 4, 4);
    bt_return(b, 4, 4);
    BT_REQUIRE(buddy_nr_free_pages() == 8);
    return bt_lists();
}

/* Run a fixture without exposing real descriptors to any synthetic test.
 * Original list nodes still point to the original head addresses, so restoring
 * the heads verbatim restores all links without touching real descriptors.
 * Even a reported test failure takes this restoration path before assertion.
 */
static bool
bt_run_isolated(const char *name, bool (*test)(void)) {
    struct Page *saved_pages = pages;
    size_t saved_npage = npage, saved_total = buddy_nr_free;
    memcpy(bt_saved_areas, buddy_areas, sizeof(buddy_areas));
    pages = bt_fake;
    npage = nbase + BT_FAKE_PAGES;
    bt_origin = (64 - nbase % 64) % 64;
    bt_name = name;
    bt_isolated = 1;
    bt_detail_a = bt_detail_b = 0;
    bool ok = test();
    pages = saved_pages;
    npage = saved_npage;
    memcpy(buddy_areas, bt_saved_areas, sizeof(buddy_areas));
    buddy_nr_free = saved_total;
    bt_isolated = 0;
    return ok;
}

static unsigned char
bt_pattern(size_t block, size_t offset) {
    return (unsigned char)(0x35 + block * 29 + offset * 7 + offset / PGSIZE);
}

static bool
bt_real_data(void) {
    bt_name = "real-memory-nonoverlap-and-data";
    bt_isolated = 0;
    size_t initial = buddy_nr_free_pages(), used = 0;
    bool ok = 1;
    memset(bt_blocks, 0, sizeof(bt_blocks));
#define BT_REAL_REQUIRE(condition) do { \
    if (!bt_expect((condition), #condition, __LINE__)) { \
        ok = 0; goto cleanup; \
    } \
} while (0)
    BT_REAL_REQUIRE(bt_lists());
    for (size_t i = 0; i < 7; i++) {
        bt_blocks[i] = buddy_alloc_pages(bt_requests[i]);
        /* Sufficient memory is mandatory for this test, never silently skip. */
        BT_REAL_REQUIRE(bt_blocks[i] != NULL);
        BT_REAL_REQUIRE(bt_allocated(bt_blocks[i], bt_sizes[i]));
        used += bt_sizes[i];
        BT_REAL_REQUIRE(buddy_nr_free_pages() == initial - used);
        for (size_t j = 0; j < i; j++) {
            BT_REAL_REQUIRE(bt_blocks[i] + bt_sizes[i] <= bt_blocks[j] ||
                            bt_blocks[j] + bt_sizes[j] <= bt_blocks[i]);
        }
        uintptr_t va = page2pa(bt_blocks[i]) + va_pa_offset;
        BT_REAL_REQUIRE(va >= KERNBASE && va < KERNTOP);
        BT_REAL_REQUIRE(bt_sizes[i] * PGSIZE <= KERNTOP - va);
        volatile unsigned char *data = (volatile unsigned char *)va;
        for (size_t k = 0; k < bt_sizes[i] * PGSIZE; k++) {
            data[k] = bt_pattern(i, k);
        }
        BT_REAL_REQUIRE(bt_lists());
    }
    /* Read only after all writes, so aliasing/overwrites cannot hide. */
    for (size_t i = 0; i < 7; i++) {
        volatile unsigned char *data = (volatile unsigned char *)(
            page2pa(bt_blocks[i]) + va_pa_offset);
        for (size_t k = 0; k < bt_sizes[i] * PGSIZE; k++) {
            bt_detail_a = i;
            bt_detail_b = k;
            BT_REAL_REQUIRE(data[k] == bt_pattern(i, k));
        }
    }
cleanup:
    for (size_t i = 7; i != 0; i--) {
        if (bt_blocks[i - 1] != NULL) {
            buddy_free_pages(bt_blocks[i - 1], bt_requests[i - 1]);
        }
    }
    if (!bt_expect(buddy_nr_free_pages() == initial,
                   "real pages returned", __LINE__)) {
        ok = 0;
    }
    if (!bt_lists()) {
        ok = 0;
    }
    if (ok) {
        struct Page *p = buddy_alloc_pages(1);
        if (!bt_expect(p != NULL, "allocation after cleanup", __LINE__)) {
            ok = 0;
        } else {
            buddy_free_pages(p, 1);
            if (!bt_lists() || buddy_nr_free_pages() != initial) {
                ok = 0;
            }
        }
    }
#undef BT_REAL_REQUIRE
    return ok;
}

static void
buddy_test_all(void) {
    cprintf("[BuddyTest] BEGIN\n");
    static const struct {
        const char *name;
        bool (*run)(void);
    } tests[] = {
        {"initialization-and-boundaries", bt_initialization},
        {"allocation-and-rounding", bt_basic},
        {"allocation-failures-unchanged-state", bt_failures},
        {"nonpartners-and-reserved-gaps", bt_nonpartners},
        {"splitting-and-merge-orders", bt_split_merge},
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        bool ok = bt_run_isolated(tests[i].name, tests[i].run);
        /* Global state has already been restored, including on failure. */
        assert(ok);
        cprintf("[BuddyTest] %s PASS\n", tests[i].name);
    }
    bool ok = bt_real_data();
    assert(ok);
    cprintf("[BuddyTest] %s PASS\n", bt_name);
    cprintf("[BuddyTest] ALL PASS\n");
}

#undef BT_REQUIRE

/* Check list accounting, physical alignment, interior state and coalescing. */
static void
buddy_check_lists(void) {
    size_t total = 0;
    for (unsigned int order = 0; order <= BUDDY_MAX_ORDER; order++) {
        list_entry_t *head = &buddy_areas[order].free_list;
        size_t subtotal = 0, size = buddy_block_size(order);
        for (list_entry_t *le = list_next(head); le != head;
             le = list_next(le)) {
            struct Page *p = le2page(le, page_link);
            assert(list_prev(list_next(le)) == le);
            assert(list_next(list_prev(le)) == le);
            assert(p >= pages && p < pages + (npage - nbase));
            assert(PageProperty(p) && p->property == size);
            assert((page2ppn(p) & (size - 1)) == 0);
            assert(size <= npage - page2ppn(p));
            for (size_t i = 0; i < size; i++) {
                assert(!PageReserved(p + i) && page_ref(p + i) == 0);
                if (i != 0) {
                    assert(!PageProperty(p + i) && p[i].property == 0);
                }
            }
            if (order < BUDDY_MAX_ORDER) {
                assert(buddy_find_free_partner(p, order) == NULL);
            }
            subtotal += size;
            assert(subtotal <= buddy_nr_free);
        }
        assert(subtotal == buddy_areas[order].nr_free);
        total += subtotal;
    }
    assert(total == buddy_nr_free_pages());
}

static void
buddy_check(void) {
    /* Bounded checks run before the original unbounded list walk. */
    buddy_test_all();
    buddy_check_lists();
    size_t initial_free = buddy_nr_free_pages();
    unsigned int initial_counts[BUDDY_MAX_ORDER + 1];
    for (unsigned int order = 0; order <= BUDDY_MAX_ORDER; order++) {
        initial_counts[order] = buddy_areas[order].nr_free;
    }

    /* Keep several sizes live together, including rounded requests. */
    const size_t requests[] = {1, 2, 3, 4, 5, 8, 9, 16};
    struct Page *allocated[sizeof(requests) / sizeof(requests[0])];
    for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); i++) {
        size_t before = buddy_nr_free_pages();
        allocated[i] = buddy_alloc_pages(requests[i]);
        if (allocated[i] != NULL) {
            unsigned int order;
            assert(buddy_request_order(requests[i], &order));
            assert(!PageProperty(allocated[i]));
            assert(allocated[i]->property == buddy_block_size(order));
            assert(before - buddy_nr_free_pages() == buddy_block_size(order));
        } else {
            assert(before == buddy_nr_free_pages());
        }
    }
    buddy_check_lists();
    for (size_t i = sizeof(requests) / sizeof(requests[0]); i != 0; i--) {
        if (allocated[i - 1] != NULL) {
            buddy_free_pages(allocated[i - 1], requests[i - 1]);
        }
    }

    /* Exhaust one page at a time, then return in reverse allocation order.
     * Allocated page_link nodes are available for this temporary test list.
     */
    list_entry_t held;
    list_init(&held);
    struct Page *p;
    size_t count = 0;
    while ((p = buddy_alloc_pages(1)) != NULL) {
        list_add_after(&held, &p->page_link);
        count++;
    }
    assert(count == initial_free && buddy_nr_free_pages() == 0);
    assert(buddy_alloc_pages(3) == NULL);
    assert(buddy_alloc_pages((size_t)-1) == NULL);
    while (!list_empty(&held)) {
        p = le2page(list_next(&held), page_link);
        list_del_init(&p->page_link);
        buddy_free_pages(p, 1);
    }
    buddy_check_lists();
    assert(buddy_nr_free_pages() == initial_free);
    for (unsigned int order = 0; order <= BUDDY_MAX_ORDER; order++) {
        assert(buddy_areas[order].nr_free == initial_counts[order]);
    }
}

const struct pmm_manager buddy_pmm_manager = {
    .name = "buddy_pmm_manager",
    .init = buddy_init,
    .init_memmap = buddy_init_memmap,
    .alloc_pages = buddy_alloc_pages,
    .free_pages = buddy_free_pages,
    .nr_free_pages = buddy_nr_free_pages,
    .check = buddy_check,
};
