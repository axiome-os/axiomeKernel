/* kernel/aluminium.c — /devices/aluminium0 char device for the Aluminium
 * graphics API.
 *
 * Provides ioctl-based command submission, GEM buffer allocation/mmap,
 * and present. Command buffers are GEM buffer objects mapped CPU-visible
 * into userspace; the application records `struct aluminium_cmd` entries
 * into the mapped BO and submits them via ALUMINIUM_IOCTL_SUBMIT.
 *
 * Hardware-agnostic: the kernel dispatcher executes each command against
 * the active gfx backend (GOP today, Intel/virtio-gpu later).
 */

#include "aluminium.h"
#include "drm/drm.h"
#include "drm/gem.h"
#include "driver.h"
#include "gfx/manager.h"
#include "printk.h"
#include "slab.h"
#include "string.h"
#include "vmm.h"
#include "sched.h"
#include <stddef.h>

/* ---- global device state ---- */

static struct drm_device *g_al_dev;
static struct device *g_al_dev_node;

/* Per-open file table. */
#define AL_MAX_FILES 16
static struct drm_file *g_al_files[AL_MAX_FILES];

static struct drm_file *al_file_alloc(void)
{
    for (int i = 0; i < AL_MAX_FILES; i++)
    {
        if (!g_al_files[i])
        {
            g_al_files[i] = (struct drm_file *)kmalloc(sizeof(struct drm_file));
            if (!g_al_files[i])
                return 0;
            __builtin_memset(g_al_files[i], 0, sizeof(struct drm_file));
            g_al_files[i]->dev = g_al_dev;
            return g_al_files[i];
        }
    }
    return 0;
}

static void al_file_free(int fd)
{
    if (fd < 0 || fd >= AL_MAX_FILES || !g_al_files[fd])
        return;
    kfree(g_al_files[fd]);
    g_al_files[fd] = 0;
}

/* ---- ioctl handlers ---- */

static long al_ioctl_create_device(void *arg)
{
    (void)arg;
    if (!g_al_dev)
        return -1;
    return 0; /* device already exists; handle is implicit via fd */
}

static long al_ioctl_gem_create(void *arg)
{
    struct aluminium_gem_create *req = (struct aluminium_gem_create *)arg;
    if (!req || !g_al_dev || req->size == 0)
        return -1;
    struct drm_gem_object *gem = drm_gem_alloc(g_al_dev, req->size);
    if (!gem)
        return -1;
    req->handle = gem->handle;
    return 0;
}

static long al_ioctl_gem_mmap(void *arg)
{
    struct aluminium_gem_mmap *req = (struct aluminium_gem_mmap *)arg;
    if (!req || !g_al_dev)
        return -1;
    struct drm_gem_object *gem = drm_gem_lookup_handle(g_al_dev, req->handle);
    if (!gem)
        return -1;
    req->offset = ((uint64_t)req->handle << 32);
    req->size = gem->size;
    return 0;
}

static long al_ioctl_gem_free(void *arg)
{
    struct aluminium_gem_create *req = (struct aluminium_gem_create *)arg;
    if (!req || !g_al_dev)
        return -1;
    struct drm_gem_object *gem = drm_gem_lookup_handle(g_al_dev, req->handle);
    if (!gem)
        return -1;
    drm_gem_free(gem);
    return 0;
}

static long al_ioctl_submit(void *arg)
{
    struct aluminium_submit *req = (struct aluminium_submit *)arg;
    if (!req || !g_al_dev)
        return -1;

    struct drm_gem_object *gem = drm_gem_lookup_handle(g_al_dev, req->gem_handle);
    if (!gem || !gem->phys)
        return -1;

    struct aluminium_cmd *cmds = (struct aluminium_cmd *)(uintptr_t)gem->phys;

    for (uint32_t i = 0; i < req->cmd_count; i++)
    {
        struct aluminium_cmd *c = &cmds[i];
        switch (c->op)
        {
            case ALUMINIUM_CMD_FILL_RECT:
                gfx_fill_rect(c->args[0], c->args[1], c->args[2],
                              c->args[3], c->args[4]);
                break;
            case ALUMINIUM_CMD_CLEAR:
                gfx_clear(c->args[0]);
                break;
            case ALUMINIUM_CMD_PRESENT:
                gfx_present_rect(c->args[0], c->args[1], c->args[2],
                                 c->args[3]);
                break;
            case ALUMINIUM_CMD_BLIT:
            case ALUMINIUM_CMD_NOP:
            default:
                break;
        }
    }

    __asm__ volatile("sfence" ::: "memory");

    if (req->fence)
    {
        /* Signal the fence — for now, fences are signaled immediately
         * since command execution is synchronous. A real GPU backend
         * would signal when the command processor completes. */
        printk("aluminium: fence %u signaled\n", req->fence);
    }

    return 0;
}

static long al_ioctl_present(void *arg)
{
    struct aluminium_present *req = (struct aluminium_present *)arg;
    if (!req || !g_al_dev)
        return -1;
    gfx_present_rect(req->x, req->y, req->w, req->h);
    return 0;
}

static long al_ioctl_get_mode(void *arg)
{
    struct aluminium_get_mode *out = (struct aluminium_get_mode *)arg;
    if (!out || !g_al_dev)
        return -1;
    uint32_t w = 0, h = 0, p = 0, b = 0;
    if (gfx_mode(&w, &h, &p, &b) != 0)
        return -1;
    out->width = w;
    out->height = h;
    out->pitch = p;
    out->bpp = b;
    out->format = 0; /* X8R8G8B8 */
    return 0;
}

static long al_ioctl_get_caps(void *arg)
{
    struct aluminium_caps *caps = (struct aluminium_caps *)arg;
    if (!caps)
        return -1;
    uint64_t bits = 0;
    if (gfx_caps(&bits) == 0)
        caps->caps = bits;
    else
        caps->caps = 0;
    caps->detail = 0;
    caps->pad = 0;
    return 0;
}

static long al_ioctl_fence_wait(void *arg)
{
    (void)arg;
    /* Software backend executes synchronously — fence is already signaled. */
    return 0;
}

static long al_ioctl_fence_signal(void *arg)
{
    (void)arg;
    return 0;
}

/* ---- main ioctl dispatcher ---- */

long aluminium_ioctl(uint32_t op, void *arg)
{
    switch (op)
    {
        case ALUMINIUM_IOCTL_CREATE_DEVICE:
            return al_ioctl_create_device(arg);
        case ALUMINIUM_IOCTL_GEM_CREATE:
            return al_ioctl_gem_create(arg);
        case ALUMINIUM_IOCTL_GEM_MMAP:
            return al_ioctl_gem_mmap(arg);
        case ALUMINIUM_IOCTL_GEM_FREE:
            return al_ioctl_gem_free(arg);
        case ALUMINIUM_IOCTL_SUBMIT:
            return al_ioctl_submit(arg);
        case ALUMINIUM_IOCTL_PRESENT:
            return al_ioctl_present(arg);
        case ALUMINIUM_IOCTL_GET_MODE:
            return al_ioctl_get_mode(arg);
        case ALUMINIUM_IOCTL_GET_CAPS:
            return al_ioctl_get_caps(arg);
        case ALUMINIUM_IOCTL_FENCE_WAIT:
            return al_ioctl_fence_wait(arg);
        case ALUMINIUM_IOCTL_FENCE_SIGNAL:
            return al_ioctl_fence_signal(arg);
        default:
            printk("aluminium: unknown ioctl 0x%x\n", op);
            return -1;
    }
}

/* ---- device ops ---- */

static long al_dev_read(struct device *d, uint64_t off, void *buf, size_t len)
{
    (void)d; (void)off; (void)buf; (void)len;
    return -1;
}

static long al_dev_write(struct device *d, uint64_t off,
                         const void *buf, size_t len)
{
    (void)d; (void)off; (void)buf; (void)len;
    return -1;
}

static long al_dev_mmap(struct device *d, uint64_t off, uint64_t virt,
                        size_t len, uint64_t flags)
{
    (void)d;
    /* Offset encoding: high 32 = GEM handle, low 32 = byte offset. */
    uint32_t handle = (uint32_t)(off >> 32);
    uint32_t byte_off = (uint32_t)(off & 0xFFFFFFFFu);
    if (!g_al_dev)
        return -1;
    struct drm_gem_object *gem = drm_gem_lookup_handle(g_al_dev, handle);
    if (!gem)
        return -1;
    if (byte_off >= gem->size)
        return -1;
    if (len > gem->size - byte_off)
        len = gem->size - byte_off;
    struct thread *t = sched_current();
    if (!t || !t->mmu)
        return -1;
    uint64_t first = (byte_off >> 12);
    uint64_t skip = byte_off & 0xFFF;
    size_t npages = (skip + len + 0xFFF) >> 12;
    if (first + npages > gem->pages)
        return -1;
    uint64_t virt_base = virt;
    for (size_t i = 0; i < npages; i++)
    {
        uint64_t pa = (uint64_t)(uintptr_t)gem->phys + (first + i) * 0x1000ULL;
        if (vmm_map_page_in(t->mmu, virt_base + i * 0x1000ULL, pa,
                            0x3 /* MMU_USER | MMU_WRITE */) < 0)
            return -1;
    }
    return 0;
}

static struct dev_ops al_dev_ops = {
    .read  = al_dev_read,
    .write = al_dev_write,
    .mmap  = al_dev_mmap,
};

/* ---- initialization ---- */

void aluminium_init(void)
{
    if (g_al_dev)
        return;

    g_al_dev = (struct drm_device *)kmalloc(sizeof(struct drm_device));
    if (!g_al_dev)
    {
        printk("aluminium: failed to allocate device\n");
        return;
    }
    __builtin_memset(g_al_dev, 0, sizeof(*g_al_dev));
    g_al_dev->name = "aluminium0";

    struct device *dev = (struct device *)kmalloc(sizeof(struct device));
    if (!dev)
    {
        kfree(g_al_dev);
        g_al_dev = 0;
        printk("aluminium: failed to allocate device node\n");
        return;
    }
    __builtin_memset(dev, 0, sizeof(*dev));
    const char *n = "aluminium0";
    for (int i = 0; n[i] && i < 31; i++)
        dev->name[i] = n[i];
    dev->major = 230;
    dev->minor = 0;
    dev->type = DEV_CHAR;
    dev->ops = al_dev_ops;
    device_register(dev);
    g_al_dev_node = dev;

    printk("aluminium: /devices/aluminium0 initialized (major 230)\n");
}
