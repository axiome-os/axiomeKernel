#ifndef AXIOME_DRM_H
#define AXIOME_DRM_H

#include <stdint.h>
#include <stddef.h>
#include "drm/drm_ioctl.h"

/* ---- forward declarations ---- */
struct drm_device;
struct drm_file;
struct drm_connector;
struct drm_crtc;
struct drm_plane;
struct drm_framebuffer;
struct drm_gem_object;

/* ---- DRM device (one per graphics card) ---- */
struct drm_device {
    const char *name;            /* "card0" */
    void *driver_private;        /* driver-specific state */
    int registered;

    /* KMS object lists */
    struct drm_connector *connectors;
    int num_connectors;
    struct drm_crtc *crtcs;
    int num_crtcs;
    struct drm_plane *planes;
    int num_planes;
    struct drm_framebuffer *fbs;
    int num_fbs;

    /* master auth */
    int master_fd;               /* fd of current DRM master, -1 if none */
    struct drm_file *master_file;
};

/* ---- DRM file (per-open instance) ---- */
struct drm_file {
    struct drm_device *dev;
    int is_master;
    void *driver_private;
};

/* ---- KMS objects ---- */
struct drm_connector {
    uint32_t id;
    uint32_t type;               /* DRM_MODE_CONNECTOR_* */
    uint32_t status;             /* 0=disconnected, 1=connected */
    struct drm_mode_info *modes;
    int num_modes;
    struct drm_encoder *encoder;
};

struct drm_crtc {
    uint32_t id;
    uint32_t fb_id;
    uint32_t x, y;
    uint32_t width, height;
    struct drm_mode_info mode;
    int mode_valid;
    struct drm_framebuffer *fb;
};

struct drm_plane {
    uint32_t id;
    uint32_t crtc_id;
    uint32_t fb_id;
};

/* ---- framebuffer (wraps a GEM object) ---- */
struct drm_framebuffer {
    uint32_t id;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t format;
    struct drm_gem_object *gem;
    uint32_t dirty;
    uint32_t dirty_x, dirty_y, dirty_w, dirty_h;
};

/* ---- GEM object ---- */
struct drm_gem_object {
    uint32_t handle;
    uint64_t size;
    uint32_t pitch;
    uint32_t height;
    void *phys;                  /* physical address (identity-mapped) */
    uint64_t pages;
    int refcount;
    int mapped;
};

/* ---- global DRM device (defined in drm.c) ---- */
extern struct drm_device *g_drm_dev;

/* ---- DRM core API ---- */
int drm_init(void);
void drm_cleanup(void);

/* GOP KMS driver init (called from drm_init). */
int gop_drm_init(void);
void gop_drm_cleanup(void);

struct drm_device *drm_device_alloc(const char *name);
void drm_device_free(struct drm_device *dev);

int drm_ioctl(struct drm_file *file, uint32_t op, void *arg);

/* Connector/crtc/fb helpers */
struct drm_connector *drm_connector_alloc(struct drm_device *dev, uint32_t type);
struct drm_crtc *drm_crtc_alloc(struct drm_device *dev);
struct drm_framebuffer *drm_framebuffer_alloc(struct drm_device *dev,
                                              uint32_t width, uint32_t height,
                                              uint32_t pitch, uint32_t bpp,
                                              uint32_t format,
                                              struct drm_gem_object *gem);
void drm_framebuffer_free(struct drm_framebuffer *fb);

/* GEM helpers */
struct drm_gem_object *drm_gem_alloc(struct drm_device *dev, uint64_t size);
void drm_gem_free(struct drm_gem_object *gem);
void *drm_gem_mmap(struct drm_gem_object *gem, uint64_t offset, size_t len);

/* C ABI for syscall layer */
#ifdef __cplusplus
extern "C" {
#endif

int drm_sys_open(const char *path, int flags);
int drm_sys_close(int fd);
long drm_sys_read(int fd, void *buf, size_t len);
long drm_sys_write(int fd, const void *buf, size_t len);
long drm_sys_mmap(int fd, uint64_t off, uint64_t virt, size_t len, uint64_t flags);
long drm_sys_ioctl(int fd, uint32_t op, void *arg);

void *drm_sys_cpu_base(void);
int drm_sys_caps(uint64_t *out_bits);

#ifdef __cplusplus
}
#endif

#endif
