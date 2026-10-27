#include <slub.h>
#include <slub_test.h>
#include <pmm.h>
#include <stdio.h>

/* Keep the pointer arrays out of the kernel's small boot stack. */
static void *objects[600];
static const size_t test_sizes[] = {
    1, 15, 16, 17, 31, 32, 33, 63, 64, 65,
    127, 128, 129, 255, 256, 257, 511, 512, 513,
    1023, 1024, 1025, 2047, 2048, 2049,
    4095, 4096, 4097, 8192, 8193, 12288
};

static unsigned char pattern(size_t object, size_t offset) {
    return (unsigned char)(0x37 + object * 23 + offset * 7 + offset / PGSIZE);
}

static void write_object(void *ptr, size_t size, size_t id) {
    assert(ptr != NULL && (uintptr_t)ptr % 16 == 0);
    unsigned char *data = ptr;
    for (size_t i = 0; i < size; i++) {
        data[i] = pattern(id, i);
    }
}

static void check_object(void *ptr, size_t size, size_t id) {
    unsigned char *data = ptr;
    for (size_t i = 0; i < size; i++) {
        assert(data[i] == pattern(id, i));
    }
}

static void fixed_cache_check(void) {
    size_t before = nr_free_pages();
    struct kmem_cache cache;
    assert(kmem_cache_init(&cache, 64));
    for (size_t i = 0; i < 40; i++) {
        objects[i] = kmem_cache_alloc(&cache);
        write_object(objects[i], 64, i);
        for (size_t j = 0; j < i; j++) {
            assert(objects[i] != objects[j]);
        }
    }
    /* Some live objects remain in this page, so the freed slot can be reused. */
    void *released = objects[10];
    kmem_cache_free(&cache, released);
    objects[10] = kmem_cache_alloc(&cache);
    assert(objects[10] == released);
    write_object(objects[10], 64, 10);
    for (size_t i = 0; i < 40; i++) {
        check_object(objects[i], 64, i);
    }
    for (size_t i = 40; i != 0; i--) {
        kmem_cache_free(&cache, objects[i - 1]);
    }
    assert(cache.slabs == NULL && nr_free_pages() == before);
}

static void multiple_slabs_check(void) {
    size_t before = nr_free_pages();
    struct kmem_cache cache;
    assert(kmem_cache_init(&cache, 16));
    /* 600 objects cannot fit into one or two 4-KiB pages. */
    for (size_t i = 0; i < 600; i++) {
        objects[i] = kmem_cache_alloc(&cache);
        write_object(objects[i], 16, i);
        for (size_t j = 0; j < i; j++) {
            assert(objects[i] != objects[j]);
        }
    }
    assert(before - nr_free_pages() >= 3);
    for (size_t i = 0; i < 600; i++) {
        check_object(objects[i], 16, i);
    }
    for (size_t parity = 0; parity < 2; parity++) {
        for (size_t i = parity; i < 600; i += 2) {
            kmem_cache_free(&cache, objects[i]);
        }
    }
    assert(cache.slabs == NULL && nr_free_pages() == before);
}

static void mixed_sizes_check(void) {
    size_t before = nr_free_pages();
    size_t count = sizeof(test_sizes) / sizeof(test_sizes[0]);
    for (size_t i = 0; i < count; i++) {
        objects[i] = kmalloc(test_sizes[i]);
        write_object(objects[i], test_sizes[i], i);
        for (size_t j = 0; j < i; j++) {
            uintptr_t a = (uintptr_t)objects[i], b = (uintptr_t)objects[j];
            assert(a + test_sizes[i] <= b || b + test_sizes[j] <= a);
        }
    }
    /* Check after all writes, so accidental overlap cannot be hidden. */
    for (size_t i = 0; i < count; i++) {
        check_object(objects[i], test_sizes[i], i);
    }
    for (size_t parity = 0; parity < 2; parity++) {
        for (size_t i = parity; i < count; i += 2) {
            check_object(objects[i], test_sizes[i], i);
            kfree(objects[i]);
        }
    }
    assert(nr_free_pages() == before);
}

static void large_objects_check(void) {
    size_t before = nr_free_pages();
    for (size_t pass = 0; pass < 3; pass++) {
        /* Payload plus header needs three pages, rounded to four by Buddy. */
        void *ptr = kmalloc(8192);
        write_object(ptr, 8192, pass);
        assert(before - nr_free_pages() >= 3);
        check_object(ptr, 8192, pass);
        kfree(ptr);
        assert(nr_free_pages() == before);
    }
}

static void edge_cases_check(void) {
    size_t before = nr_free_pages();
    struct kmem_cache cache;
    assert(!kmem_cache_init(&cache, 0));
    assert(!kmem_cache_init(&cache, 2049));
    assert(kmem_cache_init(&cache, 3));
    void *ptr = kmem_cache_alloc(&cache);
    write_object(ptr, 3, 0);
    check_object(ptr, 3, 0);
    kmem_cache_free(&cache, ptr);
    kmem_cache_free(&cache, NULL);
    assert(kmalloc(0) == NULL);
    assert(kmalloc((size_t)-1) == NULL);
    kfree(NULL);
    assert(nr_free_pages() == before);
}

static void out_of_memory_check(void) {
    size_t before = nr_free_pages();
    struct kmem_cache cache;
    assert(kmem_cache_init(&cache, 128));
    void *first = kmalloc(16);
    write_object(first, 16, 1);

    /* Hold all remaining pages using only already allocated Page list nodes. */
    list_entry_t held;
    list_init(&held);
    struct Page *page;
    while ((page = alloc_pages(1)) != NULL) {
        list_add(&held, &page->page_link);
    }
    assert(nr_free_pages() == 0);
    /* An existing slab still has free objects even when Buddy has no pages. */
    void *second = kmalloc(16);
    write_object(second, 16, 2);
    check_object(first, 16, 1);
    check_object(second, 16, 2);
    assert(kmem_cache_alloc(&cache) == NULL);
    assert(kmalloc(32) == NULL);
    assert(kmalloc(8192) == NULL);
    assert(nr_free_pages() == 0);

    while (!list_empty(&held)) {
        page = le2page(list_next(&held), page_link);
        list_del_init(&page->page_link);
        free_pages(page, 1);
    }
    check_object(first, 16, 1);
    check_object(second, 16, 2);
    kfree(second);
    kfree(first);
    assert(nr_free_pages() == before);

    /* The allocator must remain usable after the failure and cleanup. */
    void *again = kmalloc(128);
    write_object(again, 128, 3);
    check_object(again, 128, 3);
    kfree(again);
    assert(nr_free_pages() == before);
}

void slub_check(void) {
    cprintf("[SLUBTest] BEGIN\n");
    fixed_cache_check();
    cprintf("[SLUBTest] fixed-cache-and-reuse PASS\n");
    multiple_slabs_check();
    cprintf("[SLUBTest] multiple-slab-pages PASS\n");
    mixed_sizes_check();
    cprintf("[SLUBTest] mixed-sizes-and-data PASS\n");
    large_objects_check();
    cprintf("[SLUBTest] large-objects-and-page-recovery PASS\n");
    edge_cases_check();
    cprintf("[SLUBTest] zero-null-and-overflow PASS\n");
    out_of_memory_check();
    cprintf("[SLUBTest] out-of-memory-and-recovery PASS\n");
    cprintf("[SLUBTest] ALL PASS\n");
}
