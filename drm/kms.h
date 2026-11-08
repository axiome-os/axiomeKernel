#ifndef AXIOME_DRM_KMS_H
#define AXIOME_DRM_KMS_H

#include "drm/drm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* KMS mode setting for the DRM device.
   GOP backend: single connector, single crtc, single plane, fixed mode. */

/* Initialize KMS object tree on the DRM device. Returns 0 on success. */
int drm_kms_init(struct drm_device *dev);

/* Tear down KMS objects. */
void drm_kms_cleanup(struct drm_device *dev);

/* Mode set: program the crtc with a framebuffer at (x,y).
   GOP: only accepts the current firmware mode. */
int drm_kms_mode_set(struct drm_crtc *crtc, struct drm_framebuffer *fb,
                     uint32_t x, uint32_t y);

/* Page flip: make `fb` the new scanout on `crtc`.
   GOP: CPU fb_flush of the fb's backing buffer. */
int drm_kms_page_flip(struct drm_crtc *crtc, struct drm_framebuffer *fb);

/* Mark a region of a framebuffer dirty (for the next flush/present). */
int drm_kms_dirty_fb(struct drm_framebuffer *fb,
                     uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* Get the current scanout mode. */
int drm_kms_get_mode(struct drm_crtc *crtc,
                     uint32_t *w, uint32_t *h, uint32_t *pitch, uint32_t *bpp);

#ifdef __cplusplus
}
#endif

#endif
