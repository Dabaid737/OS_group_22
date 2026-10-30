#include <buddy_system.h>
#include <list.h>

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

const struct pmm_manager buddy_pmm_manager = {
    .name = "buddy_pmm_manager",
    .init = buddy_init,
    .init_memmap = buddy_init_memmap,
    .alloc_pages = buddy_alloc_pages,
    .free_pages = buddy_free_pages,
    .nr_free_pages = buddy_nr_free_pages,
};
