#ifndef __KERN_MM_SLUB_H__
#define __KERN_MM_SLUB_H__

#include <defs.h>

struct slub_slab;

/* One cache manages objects of one size, rounded up to 16-byte alignment. */
struct kmem_cache {
    size_t object_size;
    struct slub_slab *slabs;
};

/* Initialize a fresh cache. Supported object sizes are 1 through 2048 bytes. */
bool kmem_cache_init(struct kmem_cache *cache, size_t size);
void *kmem_cache_alloc(struct kmem_cache *cache);
void kmem_cache_free(struct kmem_cache *cache, void *ptr);

/* Call slub_init once, after pmm_init. All returned pointers are 16-byte aligned. */
void slub_init(void);
void *kmalloc(size_t size);
void kfree(void *ptr);

#endif
