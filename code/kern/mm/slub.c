#include <slub.h>
#include <pmm.h>
#include <string.h>

#define SLUB_ALIGN 16
#define SLUB_MAGIC 0x534c5542534c4142ULL
#define SLUB_MAX_SMALL 2048
#define SLUB_CACHE_COUNT 8
#define SLUB_BITMAP_WORDS 4

/* This header lives inside the allocated memory, not inside struct Page.
 * In particular, Buddy's Page.property must keep its original value.
 */
struct slub_slab {
    uint64_t magic;
    struct kmem_cache *cache;       /* NULL means a large kmalloc allocation. */
    struct Page *page;
    struct slub_slab *next;
    void *free_list;
    size_t inuse;
    size_t requested_pages;
    uint64_t allocated[SLUB_BITMAP_WORDS];
};

#define SLUB_HEADER_SIZE ROUNDUP(sizeof(struct slub_slab), SLUB_ALIGN)

static const size_t cache_sizes[SLUB_CACHE_COUNT] = {
    16, 32, 64, 128, 256, 512, 1024, 2048
};
static struct kmem_cache caches[SLUB_CACHE_COUNT];
static bool initialized;

static void *page_address(struct Page *page) {
    return (void *)(page2pa(page) + va_pa_offset);
}

static size_t slab_capacity(struct kmem_cache *cache) {
    return (PGSIZE - SLUB_HEADER_SIZE) / cache->object_size;
}

static struct slub_slab *pointer_slab(void *ptr) {
    return (struct slub_slab *)ROUNDDOWN((uintptr_t)ptr, PGSIZE);
}

bool kmem_cache_init(struct kmem_cache *cache, size_t size) {
    if (cache == NULL || size == 0 || size > SLUB_MAX_SMALL) {
        return 0;
    }
    cache->object_size = ROUNDUP(size, SLUB_ALIGN);
    cache->slabs = NULL;
    return 1;
}

static struct slub_slab *new_slab(struct kmem_cache *cache) {
    struct Page *page = alloc_pages(1);
    if (page == NULL) {
        return NULL;
    }
    struct slub_slab *slab = page_address(page);
    memset(slab, 0, sizeof(*slab));
    slab->magic = SLUB_MAGIC;
    slab->cache = cache;
    slab->page = page;
    slab->requested_pages = 1;

    size_t count = slab_capacity(cache);
    assert(count > 0 && count <= SLUB_BITMAP_WORDS * 64);
    char *first = (char *)slab + SLUB_HEADER_SIZE;
    /* Store the next free pointer in the unused object itself. */
    for (size_t i = count; i != 0; i--) {
        void *object = first + (i - 1) * cache->object_size;
        *(void **)object = slab->free_list;
        slab->free_list = object;
    }
    slab->next = cache->slabs;
    cache->slabs = slab;
    return slab;
}

void *kmem_cache_alloc(struct kmem_cache *cache) {
    assert(cache != NULL && cache->object_size > 0);
    assert(cache->object_size <= SLUB_MAX_SMALL);
    struct slub_slab *slab = cache->slabs;
    while (slab != NULL && slab->free_list == NULL) {
        slab = slab->next;
    }
    if (slab == NULL) {
        slab = new_slab(cache);
        if (slab == NULL) {
            return NULL;
        }
    }
    void *object = slab->free_list;
    slab->free_list = *(void **)object;
    size_t index = ((char *)object - ((char *)slab + SLUB_HEADER_SIZE)) /
                   cache->object_size;
    uint64_t mask = 1ULL << (index % 64);
    assert(!(slab->allocated[index / 64] & mask));
    slab->allocated[index / 64] |= mask;
    slab->inuse++;
    return object;
}

void kmem_cache_free(struct kmem_cache *cache, void *ptr) {
    if (ptr == NULL) {
        return;
    }
    struct slub_slab *slab = pointer_slab(ptr);
    assert(slab->magic == SLUB_MAGIC && slab->cache == cache);
    assert(cache != NULL && slab->inuse > 0);
    uintptr_t first = (uintptr_t)slab + SLUB_HEADER_SIZE;
    assert((uintptr_t)ptr >= first);
    size_t offset = (uintptr_t)ptr - first;
    assert(offset % cache->object_size == 0);
    size_t index = offset / cache->object_size;
    assert(index < slab_capacity(cache));
    uint64_t mask = 1ULL << (index % 64);
    assert(slab->allocated[index / 64] & mask);
    slab->allocated[index / 64] &= ~mask;
    *(void **)ptr = slab->free_list;
    slab->free_list = ptr;
    slab->inuse--;

    /* Return completely empty slabs immediately, so there are no cached pages. */
    if (slab->inuse == 0) {
        struct slub_slab **link = &cache->slabs;
        while (*link != slab) {
            assert(*link != NULL);
            link = &(*link)->next;
        }
        *link = slab->next;
        struct Page *page = slab->page;
        slab->magic = 0;
        free_pages(page, 1);
    }
}

void slub_init(void) {
    assert(!initialized);
    for (size_t i = 0; i < SLUB_CACHE_COUNT; i++) {
        assert(kmem_cache_init(&caches[i], cache_sizes[i]));
    }
    initialized = 1;
}

void *kmalloc(size_t size) {
    assert(initialized);
    if (size == 0) {
        return NULL;
    }
    for (size_t i = 0; i < SLUB_CACHE_COUNT; i++) {
        if (size <= cache_sizes[i]) {
            return kmem_cache_alloc(&caches[i]);
        }
    }

    /* Large objects use whole contiguous pages, with the same leading header. */
    if (size > (size_t)-1 - SLUB_HEADER_SIZE - (PGSIZE - 1)) {
        return NULL;
    }
    size_t requested = (size + SLUB_HEADER_SIZE + PGSIZE - 1) / PGSIZE;
    if (requested > nr_free_pages()) {
        return NULL;
    }
    struct Page *page = alloc_pages(requested);
    if (page == NULL) {
        return NULL;
    }
    struct slub_slab *slab = page_address(page);
    memset(slab, 0, sizeof(*slab));
    slab->magic = SLUB_MAGIC;
    slab->page = page;
    /* Buddy rounds the request internally; free with this original request. */
    slab->requested_pages = requested;
    return (char *)slab + SLUB_HEADER_SIZE;
}

void kfree(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    struct slub_slab *slab = pointer_slab(ptr);
    assert(slab->magic == SLUB_MAGIC);
    if (slab->cache != NULL) {
        kmem_cache_free(slab->cache, ptr);
    } else {
        assert(ptr == (char *)slab + SLUB_HEADER_SIZE);
        struct Page *page = slab->page;
        size_t requested = slab->requested_pages;
        slab->magic = 0;
        free_pages(page, requested);
    }
}
