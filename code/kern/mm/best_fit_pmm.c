#include <pmm.h>
#include <list.h>
#include <best_fit_pmm.h>

//lab2 function2-2 YOUR CODE
static free_area_t free_area;

#define free_list (free_area.free_list)
#define nr_free (free_area.nr_free)

static void best_fit_init(void) {
    list_init(&free_list);
    nr_free = 0;
}

// 按地址插入，并合并连续的前后空闲块
static void best_fit_insert_block(struct Page *base) {
    list_entry_t *le = list_next(&free_list);

    while (le != &free_list && le2page(le, page_link) < base) {
        le = list_next(le);
    }
    list_add_before(le, &base->page_link);

    // 与前块合并
    le = list_prev(&base->page_link);
    if (le != &free_list) {
        struct Page *prev = le2page(le, page_link);

        if (prev + prev->property == base) {
            prev->property += base->property;
            ClearPageProperty(base);
            base->property = 0;
            list_del(&base->page_link);
            base = prev;
        }
    }

    // 与后块合并
    le = list_next(&base->page_link);
    if (le != &free_list) {
        struct Page *next = le2page(le, page_link);

        if (base + base->property == next) {
            base->property += next->property;
            ClearPageProperty(next);
            next->property = 0;
            list_del(&next->page_link);
        }
    }
}

static void best_fit_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);

    for (struct Page *p = base; p != base + n; p++) {
        assert(PageReserved(p));
        p->flags = 0;
        p->property = 0;
        set_page_ref(p, 0);
    }

    base->property = n;
    SetPageProperty(base);
    nr_free += n;
    best_fit_insert_block(base);
}

static struct Page *best_fit_alloc_pages(size_t n) {
    assert(n > 0);

    if (n > nr_free) {
        return NULL;
    }

    struct Page *best = NULL;
    list_entry_t *le = &free_list;

    while ((le = list_next(le)) != &free_list) {
        struct Page *p = le2page(le, page_link);

        // 同大小时保留先遇到的低地址块
        if (p->property >= n &&
            (best == NULL || p->property < best->property)) {
            best = p;

            // 恰好匹配，已不可能找到更小的合适块
            if (p->property == n) {
                break;
            }
        }
    }

    if (best == NULL) {
        return NULL;
    }

    size_t remaining = best->property - n;
    list_entry_t *prev = list_prev(&best->page_link);
    list_del(&best->page_link);

    // 将剩余部分放回原位置
    if (remaining > 0) {
        struct Page *rest = best + n;
        rest->property = remaining;
        SetPageProperty(rest);
        list_add(prev, &rest->page_link);
    }

    nr_free -= n;
    ClearPageProperty(best);
    best->property = 0;

    return best;
}

static void best_fit_free_pages(struct Page *base, size_t n) {
    assert(n > 0);

    for (struct Page *p = base; p != base + n; p++) {
        assert(!PageReserved(p) && !PageProperty(p));
        p->flags = 0;
        p->property = 0;
        set_page_ref(p, 0);
    }

    base->property = n;
    SetPageProperty(base);
    nr_free += n;
    best_fit_insert_block(base);
}

static size_t best_fit_nr_free_pages(void) {
    return nr_free;
}

const struct pmm_manager best_fit_pmm_manager = {
    .name = "best_fit_pmm_manager",
    .init = best_fit_init,
    .init_memmap = best_fit_init_memmap,
    .alloc_pages = best_fit_alloc_pages,
    .free_pages = best_fit_free_pages,
    .nr_free_pages = best_fit_nr_free_pages,
};
