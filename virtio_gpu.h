#ifndef AXIOME_VIRTIO_GPU_H
#define AXIOME_VIRTIO_GPU_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pci_device;
struct vring_desc;   /* defined in virtio_gpu.c */
struct vring_avail;
struct vring_used;

/* Modern (non-transitional) virtio-gpu: 0x1040 + device id 16. */
#define VIRTIO_GPU_VENDOR      0x1AF4
#define VIRTIO_GPU_DEVICE      0x1050

/* ---- device status bits (virtio 1.x spec 2.1) ---- */
#define VIRTIO_STATUS_ACK                (1U << 0)
#define VIRTIO_STATUS_DRIVER             (1U << 1)
#define VIRTIO_STATUS_DRIVER_OK          (1U << 2)
#define VIRTIO_STATUS_FEATURES_OK        (1U << 3)
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET (1U << 6)
#define VIRTIO_STATUS_FAILED             (1U << 7)

/* ---- feature bits ---- */
#define VIRTIO_F_VERSION_1           32
#define VIRTIO_GPU_F_VIRGL           0
#define VIRTIO_GPU_F_EDID            1
#define VIRTIO_GPU_F_RESOURCE_UUID   2
#define VIRTIO_GPU_F_RESOURCE_BLOB   3
#define VIRTIO_GPU_F_CONTEXT_INIT    4

#define VIRTIO_GPU_FLAG_FENCE         (1 << 0)
#define VIRTIO_GPU_FLAG_INFO_RING_IDX (1 << 1)

/* ---- commands (virtio spec 5.7.6.7; values are NOT contiguous with
        the older header you had: FLUSH is 0x0104, TRANSFER_2D 0x0105 ...) ---- */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO         0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D       0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF           0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT              0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH           0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D      0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING  0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING  0x0107
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO          0x0108
#define VIRTIO_GPU_CMD_GET_CAPSET               0x0109
#define VIRTIO_GPU_CMD_GET_EDID                 0x010a

#define VIRTIO_GPU_CMD_CTX_CREATE               0x0200
#define VIRTIO_GPU_CMD_CTX_DESTROY              0x0201
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE      0x0202
#define VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE      0x0203
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D       0x0204
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D      0x0205
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D    0x0206
#define VIRTIO_GPU_CMD_SUBMIT_3D                0x0207

#define VIRTIO_GPU_CMD_UPDATE_CURSOR            0x0300
#define VIRTIO_GPU_CMD_MOVE_CURSOR              0x0301

/* ---- responses ---- */
#define VIRTIO_GPU_RESP_OK_NODATA               0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO         0x1101
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO          0x1102
#define VIRTIO_GPU_RESP_OK_CAPSET               0x1103
#define VIRTIO_GPU_RESP_OK_EDID                 0x1104
#define VIRTIO_GPU_RESP_ERR_UNSPEC              0x1200
#define VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY       0x1201
#define VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID  0x1202
#define VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID 0x1203
#define VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID  0x1204
#define VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER   0x1205

#define VIRTIO_GPU_MAX_SCANOUTS 16

/* Pixel formats. B8G8R8X8 == little-endian u32 0x00RRGGBB (our X8R8G8B8). */
enum virtio_gpu_formats {
    VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM = 1,
    VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM = 2,
    VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM = 3,
    VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM = 4,
    VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM = 67,
    VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM = 68,
    VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM = 121,
    VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM = 134,
};

#define VIRTIO_GPU_CAPSET_VIRGL  1
#define VIRTIO_GPU_CAPSET_VIRGL2 2

/* ---- wire structures (all little-endian, packed) ---- */

struct virtio_gpu_config {              /* device-specific config */
    uint32_t events_read;
    uint32_t events_clear;
    uint32_t num_scanouts;
    uint32_t num_capsets;
} __attribute__((packed));

struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t  ring_idx;
    uint8_t  padding[3];
} __attribute__((packed));

struct virtio_gpu_rect {
    uint32_t x, y, width, height;
} __attribute__((packed));

struct virtio_gpu_display_one {
    struct virtio_gpu_rect r;
    uint32_t enabled;
    uint32_t flags;
} __attribute__((packed));

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} __attribute__((packed));

struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct virtio_gpu_resource_unref {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    /* followed by nr_entries x struct virtio_gpu_mem_entry */
} __attribute__((packed));

struct virtio_gpu_resource_detach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

/* ---- 3D (virgl) ---- */
struct virtio_gpu_box {
    uint32_t x, y, z;
    uint32_t w, h, d;
} __attribute__((packed));

struct virtio_gpu_transfer_host_3d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_box box;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t level;
    uint32_t stride;
    uint32_t layer_stride;
} __attribute__((packed));

struct virtio_gpu_resource_create_3d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t target;
    uint32_t format;
    uint32_t bind;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_ctx_create {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t nlen;
    uint32_t context_init;
    char debug_name[64];
} __attribute__((packed));

struct virtio_gpu_ctx_destroy {
    struct virtio_gpu_ctrl_hdr hdr;
} __attribute__((packed));

struct virtio_gpu_ctx_resource {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_cmd_submit {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t size;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_get_capset_info {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t capset_index;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resp_capset_info {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t capset_id;
    uint32_t capset_max_version;
    uint32_t capset_max_size;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_get_capset {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t capset_id;
    uint32_t capset_version;
} __attribute__((packed));

/* ---- driver state ---- */

/* One physically-contiguous DMA allocation, mapped into kernel space. */
struct virtio_gpu_dma {
    void    *virt;
    uint64_t phys;
    uint32_t pages;
};

/* A virtio PCI capability region (common / notify / device cfg). */
struct virtio_gpu_cap {
    uint8_t  present;
    uint8_t  bar;
    uint32_t offset;
    uint32_t length;
};

struct virtio_gpu_vring {
    uint16_t idx;
    uint16_t entries;
    struct virtio_gpu_dma mem;
    struct vring_desc  *desc;
    struct vring_avail *avail;
    struct vring_used  *used;
    uint16_t avail_idx;      /* next avail slot to publish (free-running) */
    uint16_t used_idx;       /* last used index we consumed (free-running) */
    uint16_t notify_off;
    uint8_t  outstanding;    /* a timed-out command may still be in flight */
};

struct virtio_gpu_device {
    int detected;
    int initialized;
    int regs_mapped;

    uint8_t bus, dev, func;
    struct pci_device *pdev;
    uint32_t irq;

    /* transport */
    struct virtio_gpu_cap cap_common, cap_notify, cap_device;
    uint32_t notify_mult;
    volatile uint8_t *common_cfg;
    volatile uint8_t *notify_base;
    volatile uint8_t *device_cfg;

    uint64_t device_features;
    uint64_t driver_features;

    struct virtio_gpu_vring control;
    struct virtio_gpu_dma cmd;       /* command bounce buffer (device-readable) */
    struct virtio_gpu_dma resp;      /* response bounce buffer (device-writable) */

    /* display bookkeeping used by gfx/virtio_gpu_display.cpp */
    int has_display_info;
    uint32_t scanout_width, scanout_height;
    uint32_t fb_resource_id;

    /* negotiated features / config */
    int has_virgl_3d, has_edid, has_resource_blob, has_context_init;
    uint32_t num_scanouts, num_capsets;

    /* id allocators */
    uint64_t fence_id;
    uint32_t next_resource_id;
    uint32_t next_context_id;
};

/* Find the device and parse its PCI capabilities (config-space reads only;
   safe to call from probe). Returns 1 if a usable modern device exists. */
int virtio_gpu_probe(void);

/* Reset + negotiate + set up the control queue. 0 on success. */
int virtio_gpu_init(struct virtio_gpu_device *vgpu);

/* Reset the device (stops all DMA) and free the queue/buffers. */
void virtio_gpu_cleanup(struct virtio_gpu_device *vgpu);

/* Synchronous command: copies `cmd` to the command buffer, waits for the
   device, copies up to resp_len bytes of the reply to `resp` (zeroed on
   entry, so it is always safe to inspect). Returns 0 only if the device
   answered with an OK_* response; -1 otherwise. Max cmd 64 KiB, resp 16 KiB. */
int virtio_gpu_submit(struct virtio_gpu_device *vgpu,
                      void *cmd, size_t cmd_len,
                      void *resp, size_t resp_len,
                      int timeout_ms);

struct virtio_gpu_device *virtio_gpu_dev(void);

/* 3D helpers */
int virtio_gpu_create_context(uint32_t ctx_id, uint32_t context_init, const char *name);
int virtio_gpu_destroy_context(uint32_t ctx_id);
int virtio_gpu_resource_create_3d(uint32_t resource_id, uint32_t width, uint32_t height,
                                  uint32_t target, uint32_t format, uint32_t bind);
int virtio_gpu_submit_3d(uint32_t ctx_id, void *cmd, uint32_t cmd_size);
int virtio_gpu_transfer_to_host_3d(uint32_t ctx_id, uint32_t resource_id,
                                   uint64_t offset, struct virtio_gpu_box *box,
                                   uint32_t level, uint32_t stride, uint32_t layer_stride);
uint32_t virtio_gpu_alloc_resource_id(void);
uint32_t virtio_gpu_alloc_context_id(void);

#ifdef __cplusplus
}
#endif

#endif
