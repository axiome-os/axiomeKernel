/* kernel/drm/drm.c — DRM core: device, file, minor, ioctl dispatcher.
   Wraps the existing GOP framebuffer as a KMS device. */

#include "drm/drm.h"
#include "drm/drm_ioctl.h"
#include "drm/kms.h"
#include "drm/gem.h"
#include "driver.h"
#include "printk.h"
#include "string.h"
#include "slab.h"
#include "pmm.h"
#include "vmm.h"
#include "framebuffer.h"
#include "gfx/manager.h"
#include "sched.h"
#include <stddef.h>

/* ---- global DRM device (single card, GOP-backed) ---- */
struct drm_device *g_drm_dev;

/* ---- file table (per-process drm_file instances) ---- */
#define DRM_MAX_FILES 16
static struct drm_file g_drm_files[DRM_MAX_FILES];

static struct drm_file *drm_file_alloc(void)
{
    for (int i = 0; i < DRM_MAX_FILES; i++)
    {
        if (!g_drm_files[i].dev)
        {
            __builtin_memset(&g_drm_files[i], 0, sizeof(g_drm_files[i]));
            return &g_drm_files[i];
        }
    }
    return 0;
}

static void drm_file_free(struct drm_file *file)
{
    if (!file)
        return;
    if (file->is_master && g_drm_dev && g_drm_dev->master_file == file)
    {
        g_drm_dev->master_file = 0;
        g_drm_dev->master_fd = -1;
    }
    __builtin_memset(file, 0, sizeof(*file));
}

/* ---- DRM ioctl handlers ---- */

static long drm_ioctl_get_resources(struct drm_file *file, void *arg)
{
    (void)file;
    struct drm_resource *res = (struct drm_resource *)arg;
    if (!res)
        return -1;
    __builtin_memset(res, 0, sizeof(*res));
    if (!g_drm_dev)
        return -1;
    res->count_connectors = g_drm_dev->num_connectors;
    res->count_crtcs = g_drm_dev->num_crtcs;
    res->count_encoders = 0;
    res->count_fbs = g_drm_dev->num_fbs;
    if (g_drm_dev->num_crtcs > 0)
    {
        struct drm_crtc *c = g_drm_dev->crtcs;
        res->min_width = c->width ? c->width : 640;
        res->min_height = c->height ? c->height : 480;
        res->max_width = c->width ? c->width : 1920;
        res->max_height = c->height ? c->height : 1080;
    }
    return 0;
}

static long drm_ioctl_get_connector(struct drm_file *file, void *arg)
{
    (void)file;
    struct drm_connector_info *ci = (struct drm_connector_info *)arg;
    if (!ci || !g_drm_dev || g_drm_dev->num_connectors == 0)
        return -1;
    __builtin_memset(ci, 0, sizeof(*ci));
    struct drm_connector *conn = g_drm_dev->connectors;
    ci->connector_id = conn->id;
    ci->connector_type = conn->type;
    ci->connection = conn->status;
    ci->mm_count = conn->num_modes;
    ci->mm_list_ptr = 0; /* modes copied inline if needed */
    return 0;
}

static long drm_ioctl_get_crtc(struct drm_file *file, void *arg)
{
    (void)file;
    struct drm_crtc_info *cr = (struct drm_crtc_info *)arg;
    if (!cr || !g_drm_dev || g_drm_dev->num_crtcs == 0)
        return -1;
    __builtin_memset(cr, 0, sizeof(*cr));
    struct drm_crtc *c = g_drm_dev->crtcs;
    cr->crtc_id = c->id;
    cr->fb_id = c->fb_id;
    cr->x = c->x;
    cr->y = c->y;
    cr->width = c->width;
    cr->height = c->height;
    cr->mode_valid = c->mode_valid ? 1 : 0;
    return 0;
}

static long drm_ioctl_set_crtc(struct drm_file *file, void *arg)
{
    (void)file;
    struct drm_mode_crtc *req = (struct drm_mode_crtc *)arg;
    if (!req || !g_drm_dev || g_drm_dev->num_crtcs == 0)
        return -1;
    struct drm_crtc *c = g_drm_dev->crtcs;
    /* GOP: accept only the current mode. Reject mode changes. */
    if (req->mode_valid)
    {
        if (req->mode.hdisplay != c->width || req->mode.vdisplay != c->height)
            return -1;
    }
    c->fb_id = req->fb_id;
    c->x = req->x;
    c->y = req->y;
    c->width = req->mode.hdisplay ? req->mode.hdisplay : c->width;
    c->height = req->mode.vdisplay ? req->mode.vdisplay : c->height;
    c->mode_valid = req->mode_valid ? 1 : 0;
    if (req->mode_valid)
        c->mode = req->mode;
    return 0;
}

static long drm_ioctl_page_flip(struct drm_file *file, void *arg)
{
    (void)file;
    struct drm_mode_page_flip *pf = (struct drm_mode_page_flip *)arg;
    if (!pf || !g_drm_dev || g_drm_dev->num_crtcs == 0)
        return -1;
    struct drm_crtc *c = g_drm_dev->crtcs;

    /* Resolve fb_id: either a registered framebuffer or a raw GEM handle. */
    struct drm_framebuffer *fb = 0;
    struct drm_gem_object *gem = 0;
    uint32_t width = 0, height = 0, pitch = 0;

    for (int i = 0; i < g_drm_dev->num_fbs; i++)
    {
        if (g_drm_dev->fbs[i].id == pf->fb_id)
        {
            fb = &g_drm_dev->fbs[i];
            break;
        }
    }
    if (fb)
    {
        gem = fb->gem;
        width = fb->width;
        height = fb->height;
        pitch = fb->pitch ? fb->pitch : fb->width * 4;
    }
    else
    {
        gem = drm_gem_lookup_handle(g_drm_dev, pf->fb_id);
        if (!gem)
            return -1;
        width = c->width;
        height = c->height;
        pitch = gem->pitch ? gem->pitch : width * 4;
    }
    if (!gem || !gem->phys)
        return -1;

    c->fb_id = pf->fb_id;
    c->fb = fb;
    c->width = width;
    c->height = height;

    /* GOP has no hardware flip: copy the GEM buffer contents to the scanout.
       GEM phys is identity-mapped, so it is directly accessible as a kernel
       virtual address. */
    volatile uint8_t *scanout = (volatile uint8_t *)fb_addr();
    uint8_t *gem_px = (uint8_t *)(uintptr_t)gem->phys;
    if (!scanout || !gem_px)
        return -1;
    uint32_t bpp_bytes = 4; /* GOP is always 32bpp */
    for (uint32_t y = 0; y < height; y++)
    {
        __builtin_memcpy((void *)(scanout + (size_t)y * pitch),
                         gem_px + (size_t)y * pitch,
                         (size_t)width * bpp_bytes);
    }
    __asm__ volatile("sfence" ::: "memory");
    return 0;
}

static long drm_ioctl_dirtyfb(struct drm_file *file, void *arg)
{
    (void)file;
    struct drm_mode_dirtyfb *df = (struct drm_mode_dirtyfb *)arg;
    if (!df || !g_drm_dev)
        return -1;
    struct drm_framebuffer *fb = 0;
    for (int i = 0; i < g_drm_dev->num_fbs; i++)
    {
        if (g_drm_dev->fbs[i].id == df->fb_id)
        {
            fb = &g_drm_dev->fbs[i];
            break;
        }
    }
    if (!fb)
        return -1;
    /* Simple: mark whole fb dirty */
    fb->dirty = 1;
    fb->dirty_x = 0;
    fb->dirty_y = 0;
    fb->dirty_w = fb->width;
    fb->dirty_h = fb->height;
    return 0;
}

static long drm_ioctl_gem_create(struct drm_file *file, void *arg)
{
    (void)file;
    struct drm_gem_create *req = (struct drm_gem_create *)arg;
    if (!req || !g_drm_dev)
        return -1;
    if (req->size == 0)
        return -1;
    struct drm_gem_object *gem = drm_gem_alloc(g_drm_dev, req->size);
    if (!gem)
        return -1;
    req->handle = gem->handle;
    return 0;
}

static long drm_ioctl_gem_mmap(struct drm_file *file, void *arg)
{
    (void)file;
    struct drm_gem_mmap *req = (struct drm_gem_mmap *)arg;
    if (!req || !g_drm_dev)
        return -1;
    struct drm_gem_object *gem = 0;
    /* find gem by handle */
    for (int i = 0; i < DRM_MAX_FILES; i++) { (void)i; }
    /* Simple linear search: we don't have a global gem table yet.
       For now, store gems in the drm_device. */
    (void)gem;
    return -1;
}

static long drm_ioctl_gem_free(struct drm_file *file, void *arg)
{
    (void)file;
    struct drm_gem_free *req = (struct drm_gem_free *)arg;
    if (!req || !g_drm_dev)
        return -1;
    /* find and free gem by handle */
    (void)req;
    return -1;
}

static long drm_ioctl_get_mode(struct drm_file *file, void *arg)
{
    (void)file;
    if (!g_drm_dev || g_drm_dev->num_crtcs == 0)
        return -1;
    struct drm_crtc *c = g_drm_dev->crtcs;
    uint32_t *out = (uint32_t *)arg;
    out[0] = c->width;
    out[1] = c->height;
    out[2] = c->width * 4;   /* pitch */
    out[3] = 32;             /* bpp */
    out[4] = DRM_FORMAT_XRGB8888;
    return 0;
}

static long drm_ioctl_get_caps(struct drm_file *file, void *arg)
{
    (void)file;
    struct drm_get_caps *caps = (struct drm_get_caps *)arg;
    if (!caps)
        return -1;
    uint64_t bits = 0;
    if (gfx_caps(&bits) == 0)
    {
        caps->caps = bits;
    }
    else
    {
        caps->caps = 0;
    }
    const char *mode = gfx_detail_mode();
    caps->detail = (mode && mode[0] == 'd') ? 1u : 0u;
    return 0;
}

static long drm_ioctl_set_master(struct drm_file *file, void *arg)
{
    (void)arg;
    if (!g_drm_dev)
        return -1;
    if (g_drm_dev->master_file && g_drm_dev->master_file != file)
        return -1; /* EBUSY: another client is master */
    g_drm_dev->master_file = file;
    g_drm_dev->master_fd = 0; /* fd not tracked here */
    file->is_master = 1;
    printk("DRM: master set\n");
    return 0;
}

static long drm_ioctl_drop_master(struct drm_file *file, void *arg)
{
    (void)arg;
    if (!g_drm_dev || !g_drm_dev->master_file)
        return -1;
    if (g_drm_dev->master_file != file)
        return -1;
    g_drm_dev->master_file = 0;
    g_drm_dev->master_fd = -1;
    file->is_master = 0;
    printk("DRM: master dropped\n");
    return 0;
}

/* ---- main ioctl dispatcher ---- */

int drm_ioctl(struct drm_file *file, uint32_t op, void *arg)
{
    switch (op)
    {
        case DRM_IOCTL_GET_RESOURCES:
            return drm_ioctl_get_resources(file, arg);
        case DRM_IOCTL_GET_CONNECTOR:
            return drm_ioctl_get_connector(file, arg);
        case DRM_IOCTL_GET_CRTC:
            return drm_ioctl_get_crtc(file, arg);
        case DRM_IOCTL_SET_CRTC:
            return drm_ioctl_set_crtc(file, arg);
        case DRM_IOCTL_PAGE_FLIP:
            return drm_ioctl_page_flip(file, arg);
        case DRM_IOCTL_DIRTYFB:
            return drm_ioctl_dirtyfb(file, arg);
        case DRM_IOCTL_GEM_CREATE:
            return drm_ioctl_gem_create(file, arg);
        case DRM_IOCTL_GEM_MMAP:
            return drm_ioctl_gem_mmap(file, arg);
        case DRM_IOCTL_GEM_FREE:
            return drm_ioctl_gem_free(file, arg);
        case DRM_IOCTL_GET_MODE:
            return drm_ioctl_get_mode(file, arg);
        case DRM_IOCTL_GET_CAPS:
            return drm_ioctl_get_caps(file, arg);
        case DRM_IOCTL_SET_MASTER:
            return drm_ioctl_set_master(file, arg);
        case DRM_IOCTL_DROP_MASTER:
            return drm_ioctl_drop_master(file, arg);
        default:
            printk("DRM: unknown ioctl 0x%x\n", op);
            return -1;
    }
}

/* ---- C ABI for syscall layer ---- */

int drm_sys_open(const char *path, int flags)
{
    (void)path; (void)flags;
    if (!g_drm_dev)
        return -1;
    struct drm_file *file = drm_file_alloc();
    if (!file)
        return -1;
    file->dev = g_drm_dev;
    /* Return a pseudo-fd; the VFS layer will map this. For now use index. */
    for (int i = 0; i < DRM_MAX_FILES; i++)
    {
        if (&g_drm_files[i] == file)
            return i;
    }
    return -1;
}

int drm_sys_close(int fd)
{
    if (fd < 0 || fd >= DRM_MAX_FILES)
        return -1;
    drm_file_free(&g_drm_files[fd]);
    return 0;
}

long drm_sys_read(int fd, void *buf, size_t len)
{
    (void)fd; (void)buf; (void)len;
    /* GET_MODE via read for compat */
    if (len == sizeof(uint32_t) * 5)
    {
        uint32_t out[5];
        long r = drm_ioctl_get_mode(0, out);
        if (r < 0)
            return r;
        __builtin_memcpy(buf, out, sizeof(out));
        return sizeof(out);
    }
    return -1;
}

long drm_sys_write(int fd, const void *buf, size_t len)
{
    (void)fd;
    if (!g_drm_dev)
        return -1;
    if (len < sizeof(struct drm_cmd))
        return -1;
    struct drm_cmd cmd;
    __builtin_memcpy(&cmd, buf, sizeof(cmd));
    if (cmd.magic != DRM_IOCTL_MAGIC)
        return -1;
    long r = drm_ioctl(0, cmd.op, (void *)cmd.args);
    if (r < 0)
        return r;
    /* Copy results back to userspace (in-out ioctl-over-write, like dri.c).
       buf was already user-range-checked by sys_write. */
    __builtin_memcpy((void *)(uintptr_t)buf, &cmd, sizeof(cmd));
    return (long)sizeof(cmd);
}

long drm_sys_mmap(int fd, uint64_t off, uint64_t virt, size_t len, uint64_t flags)
{
    (void)fd;
    if (!g_drm_dev)
        return -1;
    /* Offset encoding: high 32 = GEM handle, low 32 = byte offset. */
    uint32_t handle = (uint32_t)(off >> 32);
    uint32_t byte_off = (uint32_t)(off & 0xFFFFFFFFu);
    struct drm_gem_object *gem = drm_gem_lookup_handle(g_drm_dev, handle);
    if (!gem)
        return -1;
    if (byte_off >= gem->size)
        return -1;
    if (len > gem->size - byte_off)
        len = (size_t)(gem->size - byte_off);
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

long drm_sys_ioctl(int fd, uint32_t op, void *arg)
{
    (void)fd;
    if (!g_drm_dev)
        return -1;
    return drm_ioctl(0, op, arg);
}

void *drm_sys_cpu_base(void)
{
    return gfx_cpu_base();
}

int drm_sys_caps(uint64_t *out_bits)
{
    return gfx_caps(out_bits);
}

/* ---- device ops for /devices/card0 (and /dev/dri/card0 via sys_open) ---- */

static long drm_dev_read(struct device *d, uint64_t off, void *buf, size_t len)
{
    (void)d; (void)off;
    return drm_sys_read(0, buf, len);
}

static long drm_dev_write(struct device *d, uint64_t off,
                          const void *buf, size_t len)
{
    (void)d; (void)off;
    return drm_sys_write(0, buf, len);
}

static long drm_dev_mmap(struct device *d, uint64_t off, uint64_t virt,
                         size_t len, uint64_t flags)
{
    (void)d;
    return drm_sys_mmap(0, off, virt, len, flags);
}

static struct dev_ops drm_dev_ops = {
    .read  = drm_dev_read,
    .write = drm_dev_write,
    .mmap  = drm_dev_mmap,
};

/* ---- initialization ---- */

int drm_init(void)
{
    if (g_drm_dev)
        return 0;
    g_drm_dev = (struct drm_device *)kmalloc(sizeof(struct drm_device));
    if (!g_drm_dev)
        return -1;
    __builtin_memset(g_drm_dev, 0, sizeof(*g_drm_dev));
    g_drm_dev->name = "card0";
    g_drm_dev->master_fd = -1;

    /* Register the char device so /devices/card0 (and /dev/dri/card0 via
       sys_open translation) resolves to the DRM device. */
    struct device *dev = (struct device *)kmalloc(sizeof(struct device));
    if (!dev)
    {
        kfree(g_drm_dev);
        g_drm_dev = 0;
        return -1;
    }
    __builtin_memset(dev, 0, sizeof(*dev));
    const char *n = "card0";
    for (int i = 0; n[i] && i < 31; i++)
        dev->name[i] = n[i];
    dev->major = 228;
    dev->minor = 0;
    dev->type = DEV_CHAR;
    dev->ops = drm_dev_ops;
    device_register(dev);
    printk("DRM: core initialized (card0 -> /devices/card0)\n");

    /* Initialize KMS + GEM + GOP driver */
    if (gop_drm_init() < 0)
    {
        printk("DRM: gop_drm_init failed\n");
    }
    return 0;
}

void drm_cleanup(void)
{
    if (!g_drm_dev)
        return;
    for (int i = 0; i < DRM_MAX_FILES; i++)
        drm_file_free(&g_drm_files[i]);
    __builtin_memset(g_drm_dev, 0, sizeof(*g_drm_dev));
    g_drm_dev = 0;
}
