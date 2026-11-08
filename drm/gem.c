/* kernel/drm/gem.c — GEM buffer objects.
   Backed by pmm frames (identity-mapped), mmap-able into userspace. */

#include "drm/drm.h"
#include "drm/gem.h"
#include "printk.h"
#include "string.h"
#include "pmm.h"
#include "vmm.h"
#include "sched.h"
#include <stddef.h>

#define GEM_MAX_OBJECTS 64

static struct drm_gem_object g_gem_objects[GEM_MAX_OBJECTS];

static struct drm_gem_object *gem_find_handle(uint32_t handle)
{
    if (handle == 0 || handle > GEM_MAX_OBJECTS)
        return 0;
    struct drm_gem_object *g = &g_gem_objects[handle - 1];
    return g->refcount > 0 ? g : 0;
}

static uint32_t gem_alloc_handle(void)
{
    for (uint32_t i = 0; i < GEM_MAX_OBJECTS; i++)
    {
        if (g_gem_objects[i].refcount == 0)
            return i + 1;
    }
    return 0;
}

struct drm_gem_object *drm_gem_alloc(struct drm_device *dev, uint64_t size)
{
    (void)dev;
    if (size == 0)
        return 0;
    uint64_t pages = (size + 0xFFF) >> 12;
    void *phys = pmm_alloc_frames(pages);
    if (!phys)
        return 0;
    uint32_t handle = gem_alloc_handle();
    if (!handle)
    {
        pmm_free_frames(phys, pages);
        return 0;
    }
    struct drm_gem_object *g = &g_gem_objects[handle - 1];
    __builtin_memset(g, 0, sizeof(*g));
    g->handle = handle;
    g->size = size;
    g->phys = phys;
    g->pages = pages;
    g->pitch = 0;
    g->height = 0;
    g->refcount = 1;
    g->mapped = 0;
    __builtin_memset(phys, 0, (size_t)size);
    printk("GEM: allocated handle=%u size=%llu pages=%llu phys=0x%lx\n",
           handle, (unsigned long long)size,
           (unsigned long long)pages, (unsigned long)(uintptr_t)phys);
    return g;
}

void drm_gem_free(struct drm_gem_object *gem)
{
    if (!gem || gem->refcount <= 0)
        return;
    gem->refcount--;
    if (gem->refcount > 0)
        return;
    if (gem->phys && gem->pages > 0)
        pmm_free_frames(gem->phys, gem->pages);
    __builtin_memset(gem, 0, sizeof(*gem));
}

void *drm_gem_mmap(struct drm_gem_object *gem, uint64_t offset, size_t len)
{
    if (!gem || !gem->phys || offset >= gem->size)
        return 0;
    if (len > gem->size - offset)
        len = (size_t)(gem->size - offset);
    struct thread *t = sched_current();
    if (!t || !t->mmu)
        return 0;
    uint64_t first = (offset >> 12);
    uint64_t skip = offset & 0xFFF;
    size_t npages = (skip + len + 0xFFF) >> 12;
    if (first + npages > gem->pages)
        return 0;
    /* Map at a fixed userspace window: use AXDRI_MAP_HINT-like address.
       For simplicity, map at offset-based hint. */
    uint64_t virt = 0x200000000000ULL + (uint64_t)gem->handle * 0x100000000ULL;
    for (size_t i = 0; i < npages; i++)
    {
        uint64_t pa = (uint64_t)(uintptr_t)gem->phys + (first + i) * 0x1000ULL;
        if (vmm_map_page_in(t->mmu, virt + i * 0x1000ULL, pa,
                            0x3 /* MMU_USER | MMU_WRITE */) < 0)
        {
            return 0;
        }
    }
    gem->mapped = 1;
    return (void *)(virt + skip);
}

struct drm_gem_object *drm_gem_lookup_handle(struct drm_device *dev, uint32_t handle)
{
    (void)dev;
    return gem_find_handle(handle);
}
