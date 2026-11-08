#ifndef AXIOME_DRM_IOCTL_H
#define AXIOME_DRM_IOCTL_H

/* axiomeOS DRM ioctl definitions.
   Freestanding (stdint only); shared by kernel/drm/* and userspace tests.
   Modeled on Linux DRM concepts but our own numbers — no Wayland, no
   compatibility with Linux UAPI. */

#include <stdint.h>

/* ---- ioctl numbers (our own namespace) ---- */
#define DRM_IOCTL_BASE           0x40
#define DRM_IOCTL_GET_RESOURCES  (DRM_IOCTL_BASE + 0x00)
#define DRM_IOCTL_GET_CONNECTOR  (DRM_IOCTL_BASE + 0x01)
#define DRM_IOCTL_GET_CRTC       (DRM_IOCTL_BASE + 0x02)
#define DRM_IOCTL_SET_CRTC       (DRM_IOCTL_BASE + 0x03)
#define DRM_IOCTL_PAGE_FLIP      (DRM_IOCTL_BASE + 0x04)
#define DRM_IOCTL_DIRTYFB        (DRM_IOCTL_BASE + 0x05)
#define DRM_IOCTL_GEM_CREATE     (DRM_IOCTL_BASE + 0x06)
#define DRM_IOCTL_GEM_MMAP       (DRM_IOCTL_BASE + 0x07)
#define DRM_IOCTL_GEM_FREE       (DRM_IOCTL_BASE + 0x08)
#define DRM_IOCTL_GET_MODE       (DRM_IOCTL_BASE + 0x09)
#define DRM_IOCTL_GET_CAPS       (DRM_IOCTL_BASE + 0x0A)
#define DRM_IOCTL_SET_MASTER     (DRM_IOCTL_BASE + 0x0B)
#define DRM_IOCTL_DROP_MASTER    (DRM_IOCTL_BASE + 0x0C)

/* ---- capability bits (mirrors gfx::GfxCap) ---- */
#define DRM_CAP_HW_FILL    (1ull << 0)
#define DRM_CAP_HW_BLIT    (1ull << 1)
#define DRM_CAP_HW_FLIP    (1ull << 2)
#define DRM_CAP_3D         (1ull << 3)
#define DRM_CAP_ALPHA      (1ull << 4)
#define DRM_CAP_COMPOSITOR (1ull << 5)
#define DRM_CAP_BLUR       (1ull << 6)
#define DRM_CAP_SHADOWS    (1ull << 7)
#define DRM_CAP_SHADERS    (1ull << 8)
#define DRM_CAP_VSYNC      (1ull << 9)
#define DRM_CAP_HW_CURSOR  (1ull << 10)
#define DRM_CAP_SCALE      (1ull << 11)
#define DRM_CAP_YUV        (1ull << 12)

/* ---- pixel formats ---- */
#define DRM_FORMAT_XRGB8888 0  /* 0x00RRGGBB canonical */
#define DRM_FORMAT_XBGR8888 1  /* 0x00BBGGRR canonical */

/* ---- GET_RESOURCES: list of connectors, crtcs, encoders, framebuffers ---- */
struct drm_resource {
    uint32_t count_connectors;
    uint32_t count_crtcs;
    uint32_t count_encoders;
    uint32_t count_fbs;
    uint32_t min_width;
    uint32_t min_height;
    uint32_t max_width;
    uint32_t max_height;
};

/* ---- GET_CONNECTOR: connector info + modes ---- */
struct drm_connector_info {
    uint32_t connector_id;
    uint32_t connector_type;   /* 0=builtin, 1=VGA, 2=HDMI, 3=DP */
    uint32_t connection;       /* 0=disconnected, 1=connected */
    uint32_t mm_count;         /* number of modes */
    uint32_t mm_list_ptr;      /* offset into buffer for drm_mode_info */
    uint32_t pad;
};

/* ---- drm_mode_info: display mode ---- */
struct drm_mode_info {
    uint32_t clock;            /* pixel clock in kHz */
    uint16_t hdisplay;
    uint16_t hsync_start;
    uint16_t hsync_end;
    uint16_t htotal;
    uint16_t hskew;
    uint16_t vdisplay;
    uint16_t vsync_start;
    uint16_t vsync_end;
    uint16_t vtotal;
    uint16_t vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char name[32];
};

/* ---- GET_CRTC: crtc state + attached fb ---- */
struct drm_crtc_info {
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t mode_valid;
    uint32_t gamma_size;
};

/* ---- SET_CRTC: modeset ---- */
struct drm_mode_crtc {
    uint32_t set_connectors_ptr;   /* array of connector ids */
    uint32_t count_connectors;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x;
    uint32_t y;
    uint32_t mode_valid;
    struct drm_mode_info mode;
    uint32_t pad[3];
};

/* ---- PAGE_FLIP: swap fb to scanout ---- */
struct drm_mode_page_flip {
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t flags;              /* 0=blocking, 1=async */
    uint32_t pad;
};

/* ---- DIRTYFB: mark framebuffer region dirty ---- */
struct drm_mode_dirtyfb {
    uint32_t fb_id;
    uint32_t flags;
    uint32_t color;
    uint32_t clipping;
    uint32_t num_clips;
    uint32_t clips_ptr;          /* array of {x1,y1,x2,y2} */
};

/* ---- GEM_CREATE: allocate dumb buffer ---- */
struct drm_gem_create {
    uint32_t size;               /* bytes */
    uint32_t handle;             /* out */
    uint32_t pad;
};

/* ---- GEM_MMAP: get mmap offset for handle ---- */
struct drm_gem_mmap {
    uint32_t handle;
    uint64_t offset;   /* out: (handle << 32) | byte_off, passed to mmap() */
    uint64_t size;     /* out */
};

/* ---- GEM_FREE ---- */
struct drm_gem_free {
    uint32_t handle;
    uint32_t pad;
};

/* ---- GET_CAPS ---- */
struct drm_get_caps {
    uint64_t caps;               /* out */
    uint32_t detail;             /* out: 0=simplified 1=detailed */
    uint32_t pad;
};

/* ---- ioctl command wrapper (write()-compatible for now) ---- */
/* One command per write() on /dev/dri/card0. */
struct drm_cmd {
    uint32_t magic;              /* DRM_IOCTL_MAGIC */
    uint32_t op;                 /* DRM_IOCTL_* */
    uint32_t args[6];            /* op-specific, some in/out */
};

#define DRM_IOCTL_MAGIC 0x44524D00u /* "DRM\0" */
#define DRM_CMD_SIZE 32

#endif
