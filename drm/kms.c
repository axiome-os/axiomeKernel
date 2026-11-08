/* kernel/drm/kms.c — KMS objects: connector, crtc, plane, framebuffer.
   GOP backend: single connector/crtc/plane, adopts UEFI GOP mode. */

#include "drm/drm.h"
#include "drm/kms.h"
#include "drm/gem.h"
#include "driver.h"
#include "slab.h"
#include "printk.h"
#include "string.h"
#include "framebuffer.h"
#include "gfx/manager.h"
#include <stddef.h>

/* ---- connector ---- */

struct drm_connector *drm_connector_alloc(struct drm_device *dev, uint32_t type)
{
    if (!dev)
        return 0;
    struct drm_connector *c = (struct drm_connector *)kmalloc(sizeof(*c));
    if (!c)
        return 0;
    __builtin_memset(c, 0, sizeof(*c));
    c->id = dev->num_connectors + 1;
    c->type = type;
    c->status = 1; /* connected */
    c->encoder = 0;
    c->modes = 0;
    c->num_modes = 0;
    return c;
}

/* ---- crtc ---- */

struct drm_crtc *drm_crtc_alloc(struct drm_device *dev)
{
    if (!dev)
        return 0;
    struct drm_crtc *c = (struct drm_crtc *)kmalloc(sizeof(*c));
    if (!c)
        return 0;
    __builtin_memset(c, 0, sizeof(*c));
    c->id = dev->num_crtcs + 1;
    c->fb_id = 0;
    c->x = 0;
    c->y = 0;
    c->width = 0;
    c->height = 0;
    c->mode_valid = 0;
    c->fb = 0;
    return c;
}

/* ---- framebuffer ---- */

struct drm_framebuffer *drm_framebuffer_alloc(struct drm_device *dev,
                                              uint32_t width, uint32_t height,
                                              uint32_t pitch, uint32_t bpp,
                                              uint32_t format,
                                              struct drm_gem_object *gem)
{
    (void)dev;
    if (!gem)
        return 0;
    struct drm_framebuffer *fb = (struct drm_framebuffer *)kmalloc(sizeof(*fb));
    if (!fb)
        return 0;
    __builtin_memset(fb, 0, sizeof(*fb));
    fb->id = 1; /* single fb for now */
    fb->width = width;
    fb->height = height;
    fb->pitch = pitch;
    fb->bpp = bpp;
    fb->format = format;
    fb->gem = gem;
    fb->dirty = 0;
    fb->dirty_x = 0;
    fb->dirty_y = 0;
    fb->dirty_w = 0;
    fb->dirty_h = 0;
    return fb;
}

void drm_framebuffer_free(struct drm_framebuffer *fb)
{
    if (!fb)
        return;
    if (fb->gem)
        drm_gem_free(fb->gem);
    __builtin_memset(fb, 0, sizeof(*fb));
}

/* ---- KMS init/cleanup ---- */

int drm_kms_init(struct drm_device *dev)
{
    if (!dev)
        return -1;

    /* Single connector (builtin panel) */
    struct drm_connector *conn = drm_connector_alloc(dev, 0);
    if (!conn)
        return -1;
    dev->connectors = conn;
    dev->num_connectors = 1;

    /* Single crtc */
    struct drm_crtc *crtc = drm_crtc_alloc(dev);
    if (!crtc)
        return -1;
    dev->crtcs = crtc;
    dev->num_crtcs = 1;

    /* Query GOP mode for initial dimensions */
    uint32_t w = 0, h = 0, pitch = 0, bpp = 0;
    if (gfx_mode(&w, &h, &pitch, &bpp) < 0)
    {
        w = 640; h = 480; pitch = w * 4; bpp = 32;
    }
    crtc->width = w;
    crtc->height = h;
    crtc->mode_valid = 1;

    printk("DRM: KMS initialized (%ux%u @ %ubpp)\n", w, h, bpp);
    return 0;
}

void drm_kms_cleanup(struct drm_device *dev)
{
    if (!dev)
        return;
    for (int i = 0; i < dev->num_connectors; i++)
    {
        __builtin_memset(&dev->connectors[i], 0, sizeof(dev->connectors[i]));
    }
    dev->num_connectors = 0;
    for (int i = 0; i < dev->num_crtcs; i++)
    {
        __builtin_memset(&dev->crtcs[i], 0, sizeof(dev->crtcs[i]));
    }
    dev->num_crtcs = 0;
    for (int i = 0; i < dev->num_fbs; i++)
    {
        drm_framebuffer_free(&dev->fbs[i]);
    }
    dev->num_fbs = 0;
}

/* ---- mode set ---- */

int drm_kms_mode_set(struct drm_crtc *crtc, struct drm_framebuffer *fb,
                     uint32_t x, uint32_t y)
{
    if (!crtc || !fb)
        return -1;
    crtc->fb = fb;
    crtc->fb_id = fb->id;
    crtc->x = x;
    crtc->y = y;
    crtc->width = fb->width;
    crtc->height = fb->height;
    crtc->mode_valid = 1;
    return 0;
}

/* ---- page flip ---- */

int drm_kms_page_flip(struct drm_crtc *crtc, struct drm_framebuffer *fb)
{
    if (!crtc || !fb)
        return -1;
    crtc->fb = fb;
    crtc->fb_id = fb->id;
    crtc->width = fb->width;
    crtc->height = fb->height;
    /* GOP: flush back buffer to scanout */
    fb_flush();
    return 0;
}

/* ---- dirty fb ---- */

int drm_kms_dirty_fb(struct drm_framebuffer *fb,
                     uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!fb || w == 0 || h == 0)
        return -1;
    fb->dirty = 1;
    fb->dirty_x = x;
    fb->dirty_y = y;
    fb->dirty_w = w;
    fb->dirty_h = h;
    return 0;
}

/* ---- get mode ---- */

int drm_kms_get_mode(struct drm_crtc *crtc,
                     uint32_t *w, uint32_t *h, uint32_t *pitch, uint32_t *bpp)
{
    if (!crtc)
        return -1;
    if (w) *w = crtc->width;
    if (h) *h = crtc->height;
    if (pitch) *pitch = crtc->width * 4;
    if (bpp) *bpp = 32;
    return 0;
}
