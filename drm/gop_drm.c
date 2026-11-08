/* kernel/drm/gop_drm.c — GOP as a DRM KMS driver.
   Single connector/crtc/plane, adopts UEFI GOP mode, CPU present via fb_flush.
   No acceleration: caps are 0 (simplified mode). */

#include "drm/drm.h"
#include "drm/kms.h"
#include "drm/gem.h"
#include "printk.h"
#include "framebuffer.h"
#include "gfx/manager.h"
#include "hal/hal_bootinfo.h"
#include <stddef.h>

/* ---- GOP driver state ---- */

static struct drm_device *g_gop_dev;
static struct drm_framebuffer *g_gop_fb;
static struct drm_gem_object *g_gop_scanout_gem;

/* ---- KMS driver ops (called from drm core ioctl handlers) ---- */

/* Get resources: report single connector, single crtc. */
static int gop_get_resources(struct drm_device *dev, struct drm_resource *res)
{
    if (!dev || !res)
        return -1;
    __builtin_memset(res, 0, sizeof(*res));
    res->count_connectors = dev->num_connectors;
    res->count_crtcs = dev->num_crtcs;
    res->count_encoders = 0;
    res->count_fbs = dev->num_fbs;
    uint32_t w = 0, h = 0, pitch = 0, bpp = 0;
    if (gfx_mode(&w, &h, &pitch, &bpp) == 0)
    {
        res->min_width = w;
        res->min_height = h;
        res->max_width = w;
        res->max_height = h;
    }
    else
    {
        res->min_width = 640;
        res->min_height = 480;
        res->max_width = 1920;
        res->max_height = 1080;
    }
    return 0;
}

/* Get connector info. */
static int gop_get_connector(struct drm_device *dev, uint32_t id,
                             struct drm_connector_info *ci)
{
    if (!dev || !ci || dev->num_connectors == 0)
        return -1;
    struct drm_connector *c = dev->connectors;
    if (c->id != id)
        return -1;
    __builtin_memset(ci, 0, sizeof(*ci));
    ci->connector_id = c->id;
    ci->connector_type = c->type; /* 0 = builtin */
    ci->connection = c->status;   /* 1 = connected */
    ci->mm_count = c->num_modes;
    return 0;
}

/* Get CRTC state. */
static int gop_get_crtc(struct drm_device *dev, uint32_t id,
                        struct drm_crtc_info *cr)
{
    if (!dev || !cr || dev->num_crtcs == 0)
        return -1;
    struct drm_crtc *c = dev->crtcs;
    if (c->id != id)
        return -1;
    __builtin_memset(cr, 0, sizeof(*cr));
    cr->crtc_id = c->id;
    cr->fb_id = c->fb_id;
    cr->x = c->x;
    cr->y = c->y;
    cr->width = c->width;
    cr->height = c->height;
    cr->mode_valid = c->mode_valid ? 1 : 0;
    return 0;
}

/* Set CRTC (modeset). GOP: only accepts current mode. */
static int gop_set_crtc(struct drm_device *dev, struct drm_mode_crtc *req)
{
    (void)dev;
    if (!req || !g_gop_dev || g_gop_dev->num_crtcs == 0)
        return -1;
    struct drm_crtc *c = g_gop_dev->crtcs;
    if (req->mode_valid)
    {
        if (req->mode.hdisplay != c->width ||
            req->mode.vdisplay != c->height)
            return -1;
    }
    c->fb_id = req->fb_id;
    c->x = req->x;
    c->y = req->y;
    if (req->mode.hdisplay && req->mode.vdisplay)
    {
        c->width = req->mode.hdisplay;
        c->height = req->mode.vdisplay;
    }
    c->mode_valid = req->mode_valid ? 1 : 0;
    if (req->mode_valid)
        c->mode = req->mode;
    return 0;
}

/* Page flip: flush fb to scanout. */
static int gop_page_flip(struct drm_device *dev, uint32_t crtc_id,
                         uint32_t fb_id)
{
    (void)dev; (void)crtc_id;
    struct drm_framebuffer *fb = 0;
    for (int i = 0; i < dev->num_fbs; i++)
    {
        if (dev->fbs[i].id == fb_id)
        {
            fb = &dev->fbs[i];
            break;
        }
    }
    if (!fb)
        return -1;
    fb_flush();
    return 0;
}

/* Dirty fb: mark region for flush. */
static int gop_dirty_fb(struct drm_device *dev, uint32_t fb_id,
                        uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    (void)dev;
    struct drm_framebuffer *fb = 0;
    for (int i = 0; i < g_gop_dev->num_fbs; i++)
    {
        if (g_gop_dev->fbs[i].id == fb_id)
        {
            fb = &g_gop_dev->fbs[i];
            break;
        }
    }
    if (!fb)
        return -1;
    fb->dirty = 1;
    fb->dirty_x = x;
    fb->dirty_y = y;
    fb->dirty_w = w;
    fb->dirty_h = h;
    return 0;
}

/* Get mode. */
static int gop_get_mode(struct drm_device *dev,
                        uint32_t *w, uint32_t *h, uint32_t *pitch, uint32_t *bpp)
{
    (void)dev;
    return gfx_mode(w, h, pitch, bpp);
}

/* Get caps. */
static int gop_get_caps(struct drm_device *dev, uint64_t *bits, int *detail)
{
    (void)dev;
    if (gfx_caps(bits) < 0)
        *bits = 0;
    const char *m = gfx_detail_mode();
    *detail = (m && m[0] == 'd') ? 1 : 0;
    return 0;
}

/* ---- driver init: create KMS objects + scanout GEM + fb ---- */

int gop_drm_init(void)
{
    if (!g_drm_dev)
        return -1;
    g_gop_dev = g_drm_dev;

    /* Initialize KMS object tree (connector + crtc) */
    if (drm_kms_init(g_gop_dev) < 0)
        return -1;

    /* Query GOP mode */
    uint32_t w = 0, h = 0, pitch = 0, bpp = 0;
    if (gfx_mode(&w, &h, &pitch, &bpp) < 0)
    {
        w = 640; h = 480; pitch = w * 4; bpp = 32;
    }

    /* Allocate a GEM object for the scanout framebuffer */
    uint64_t fb_size = (uint64_t)pitch * h;
    g_gop_scanout_gem = drm_gem_alloc(g_gop_dev, fb_size);
    if (!g_gop_scanout_gem)
        return -1;
    g_gop_scanout_gem->pitch = pitch;
    g_gop_scanout_gem->height = h;

    /* Create a DRM framebuffer wrapping the GEM object */
    g_gop_fb = drm_framebuffer_alloc(g_gop_dev, w, h, pitch, bpp,
                                     DRM_FORMAT_XRGB8888,
                                     g_gop_scanout_gem);
    if (!g_gop_fb)
        return -1;

    /* Register the fb in the device table */
    g_gop_dev->fbs = g_gop_fb;
    g_gop_dev->num_fbs = 1;

    /* Set the crtc to use this fb */
    struct drm_crtc *crtc = g_gop_dev->crtcs;
    drm_kms_mode_set(crtc, g_gop_fb, 0, 0);

    printk("DRM: GOP KMS driver active (%ux%u pitch=%u)\n", w, h, pitch);
    return 0;
}

void gop_drm_cleanup(void)
{
    if (g_gop_fb)
        drm_framebuffer_free(g_gop_fb);
    g_gop_fb = 0;
    if (g_gop_scanout_gem)
        drm_gem_free(g_gop_scanout_gem);
    g_gop_scanout_gem = 0;
    drm_kms_cleanup(g_gop_dev);
    g_gop_dev = 0;
}

/* ---- Exposed ops for drm core ioctl dispatch ---- */

struct gop_drm_ops {
    int (*get_resources)(struct drm_device *, struct drm_resource *);
    int (*get_connector)(struct drm_device *, uint32_t, struct drm_connector_info *);
    int (*get_crtc)(struct drm_device *, uint32_t, struct drm_crtc_info *);
    int (*set_crtc)(struct drm_device *, struct drm_mode_crtc *);
    int (*page_flip)(struct drm_device *, uint32_t, uint32_t);
    int (*dirty_fb)(struct drm_device *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    int (*get_mode)(struct drm_device *, uint32_t *, uint32_t *, uint32_t *, uint32_t *);
    int (*get_caps)(struct drm_device *, uint64_t *, int *);
};

static struct gop_drm_ops g_gop_ops = {
    .get_resources = gop_get_resources,
    .get_connector = gop_get_connector,
    .get_crtc = gop_get_crtc,
    .set_crtc = gop_set_crtc,
    .page_flip = gop_page_flip,
    .dirty_fb = gop_dirty_fb,
    .get_mode = gop_get_mode,
    .get_caps = gop_get_caps,
};

struct gop_drm_ops *gop_drm_ops_get(void)
{
    return &g_gop_ops;
}
