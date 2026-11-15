#include "virtio_gpu_display.h"
#include "../hal/hal.h"
extern "C" {
#include "../printk.h"
#include "virtio_gpu.h"
#include "../slab.h"
#include "../string.h"
#include "../pci.h"
#include "../pmm.h"
}
#include "../vmm.h"
#include "../framebuffer.h"

namespace gfx {

VirtioGpuDisplay::VirtioGpuDisplay() = default;

VirtioGpuDisplay *virtio_gpu_display_create(void)
{
    alignas(VirtioGpuDisplay) static unsigned char storage[sizeof(VirtioGpuDisplay)];
    return new (storage) VirtioGpuDisplay();
}

bool VirtioGpuDisplay::probe()
{
    if (!virtio_gpu_probe())
        return false;
    struct virtio_gpu_device *d = virtio_gpu_dev();
    printk("gfx: virtio-gpu probed (PCI %02x:%02x.%u)\n",
           d->bus, d->dev, d->func);
    return true;
}

/* Unmap every virtual page of the framebuffer and return the frames.
   Callers must have reset the device (or unref'd the resource) first so the
   host can no longer read this memory. */
void VirtioGpuDisplay::release_framebuffer(void)
{
    if (fb_) {
        for (uint32_t i = 0; i < fb_pages_; i++)
            vmm_unmap_page((uint64_t)(uintptr_t)fb_ + (uint64_t)i * PAGE_SIZE);
        fb_ = nullptr;
    }
    if (fb_phys_ && fb_pages_)
        pmm_free_frames((void *)(uintptr_t)fb_phys_, fb_pages_);
    fb_phys_ = 0;
    fb_pages_ = 0;
    fb_size_ = 0;
    fb_resource_id_ = 0;
    scanout_resource_id_ = 0;
}

bool VirtioGpuDisplay::present_flush(const GfxRect &dirty)
{
    if (!ready_ || !fb_)
        return false;

    struct virtio_gpu_device *vgpu = virtio_gpu_dev();

    uint32_t x = dirty.x, y = dirty.y, w = dirty.w, h = dirty.h;
    if (w == 0 || h == 0) {
        x = 0; y = 0; w = cur_.width; h = cur_.height;   /* full frame */
    }
    if (x >= cur_.width || y >= cur_.height)
        return true;                                     /* fully off-screen */
    if (x + w > cur_.width)  w = cur_.width - x;
    if (y + h > cur_.height) h = cur_.height - y;

    /* Only the resource we own has guest backing. If a client flipped the
       scanout to its own resource, that client is responsible for its
       TRANSFER; we just flush what is on screen. */
    if (scanout_resource_id_ == fb_resource_id_) {
        struct virtio_gpu_transfer_to_host_2d xfer = {};
        xfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
        xfer.r.x = x;
        xfer.r.y = y;
        xfer.r.width = w;
        xfer.r.height = h;
        xfer.offset = (uint64_t)y * cur_.pitch + (uint64_t)x * 4;
        xfer.resource_id = fb_resource_id_;
        struct virtio_gpu_ctrl_hdr resp;
        if (virtio_gpu_submit(vgpu, &xfer, sizeof(xfer), &resp, sizeof(resp), 1000) < 0)
            return false;   /* the driver already logged (rate-limited) */
    }

    struct virtio_gpu_resource_flush cmd = {};
    cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd.r.x = x;
    cmd.r.y = y;
    cmd.r.width = w;
    cmd.r.height = h;
    cmd.resource_id = scanout_resource_id_;

    struct virtio_gpu_ctrl_hdr resp;
    return virtio_gpu_submit(vgpu, &cmd, sizeof(cmd), &resp, sizeof(resp), 1000) == 0;
}

bool VirtioGpuDisplay::create_framebuffer_resource(void)
{
    struct virtio_gpu_device *vgpu = virtio_gpu_dev();

    fb_size_ = cur_.pitch * cur_.height;
    fb_pages_ = (fb_size_ + PAGE_SIZE - 1) >> PAGE_SHIFT;

    /* Physically contiguous backing (so one attach entry is enough), below
       4 GiB when possible. */
    void *pages = pmm_alloc_frames_high(fb_pages_, 0x100000000ULL);
    if (!pages)
        pages = pmm_alloc_frames(fb_pages_);
    if (!pages) {
        printk("virtio-gpu: failed to alloc backing pages\n");
        fb_pages_ = 0;
        fb_size_ = 0;
        return false;
    }
    fb_phys_ = (uint64_t)(uintptr_t)pages;

    /* Write-back: x86 DMA is cache-coherent, CPU stores stay fast. */
    fb_ = vmm_mmap_phys(fb_phys_, fb_pages_, MMU_WRITE);
    if (!fb_) {
        printk("virtio-gpu: failed to map backing pages\n");
        release_framebuffer();
        return false;
    }
    memset(fb_, 0, fb_size_);

    /* Fresh resource id; keep away from 1..9 in case firmware left some. */
    fb_resource_id_ = virtio_gpu_alloc_resource_id();
    if (fb_resource_id_ < 10)
        fb_resource_id_ += 10;
    vgpu->fb_resource_id = fb_resource_id_;
    scanout_resource_id_ = fb_resource_id_;

    /* 1. RESOURCE_CREATE_2D (must precede ATTACH_BACKING). */
    {
        struct virtio_gpu_resource_create_2d cre = {};
        cre.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
        cre.resource_id = fb_resource_id_;
        cre.format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;   /* == X8R8G8B8 */
        cre.width = cur_.width;
        cre.height = cur_.height;
        struct virtio_gpu_ctrl_hdr resp;
        if (virtio_gpu_submit(vgpu, &cre, sizeof(cre), &resp, sizeof(resp), 1000) < 0) {
            printk("virtio-gpu: resource_create_2d %u %ux%u failed (resp 0x%x)\n",
                   fb_resource_id_, cur_.width, cur_.height, resp.type);
            release_framebuffer();
            return false;
        }
    }

    /* 2. RESOURCE_ATTACH_BACKING: one entry covering the whole buffer. */
    {
        struct {
            struct virtio_gpu_resource_attach_backing hdr;
            struct virtio_gpu_mem_entry entry;
        } __attribute__((packed)) cmd = {};
        cmd.hdr.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
        cmd.hdr.resource_id = fb_resource_id_;
        cmd.hdr.nr_entries = 1;
        cmd.entry.addr = fb_phys_;
        cmd.entry.length = fb_size_;

        struct virtio_gpu_ctrl_hdr resp;
        if (virtio_gpu_submit(vgpu, &cmd, sizeof(cmd), &resp, sizeof(resp), 2000) < 0) {
            printk("virtio-gpu: attach_backing res %u failed (resp 0x%x)\n",
                   fb_resource_id_, resp.type);
            /* No backing was attached, so unref'ing then freeing is safe. */
            struct virtio_gpu_resource_unref unref = {};
            unref.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
            unref.resource_id = fb_resource_id_;
            struct virtio_gpu_ctrl_hdr resp2;
            virtio_gpu_submit(vgpu, &unref, sizeof(unref), &resp2, sizeof(resp2), 1000);
            release_framebuffer();
            return false;
        }
    }
    printk("virtio-gpu: resource %u %ux%u backed by 0x%lx (%u bytes)\n",
           fb_resource_id_, cur_.width, cur_.height,
           (unsigned long)fb_phys_, fb_size_);
    return true;
}

bool VirtioGpuDisplay::setup_scanout(void)
{
    struct virtio_gpu_set_scanout cmd = {};
    cmd.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    cmd.r.x = 0;
    cmd.r.y = 0;
    cmd.r.width = cur_.width;
    cmd.r.height = cur_.height;
    cmd.scanout_id = 0;
    cmd.resource_id = scanout_resource_id_;

    struct virtio_gpu_ctrl_hdr resp;
    if (virtio_gpu_submit(virtio_gpu_dev(), &cmd, sizeof(cmd),
                          &resp, sizeof(resp), 1000) < 0) {
        printk("virtio-gpu: set_scanout res %u failed (resp 0x%x)\n",
               scanout_resource_id_, resp.type);
        return false;
    }
    printk("virtio-gpu: scanout 0 -> resource %u %ux%u\n",
           scanout_resource_id_, cur_.width, cur_.height);
    return true;
}

/* QEMU stops refreshing the GOP surface at SET_SCANOUT. Seed the new
   resource from the console back-buffer first, when the modes match, so
   the handoff is the current splash instead of the memset zeros. */
void VirtioGpuDisplay::copy_console_pixels(void)
{
    uint8_t *src = fb_back_buffer_for_gfx();
    if (!fb_ || !src || cur_.width == 0 || cur_.height == 0)
        return;
    if (cur_.pitch < cur_.width * 4)
        return;
    if (fb_width() != cur_.width || fb_height() != cur_.height)
        return;
    uint32_t src_pitch = fb_pitch();
    uint32_t row = cur_.width * 4;
    if (src_pitch < row)
        return;
    uint8_t *dst = (uint8_t *)fb_;
    for (uint32_t y = 0; y < cur_.height; y++)
        memcpy(dst + (size_t)y * cur_.pitch, src + (size_t)y * src_pitch, row);
}

bool VirtioGpuDisplay::init(const GfxMode &mode)
{
    if (ready_)
        return true;

    struct virtio_gpu_device *d = virtio_gpu_dev();
    if (!d->initialized && virtio_gpu_init(d) < 0)
        return false;

    if (!d->has_display_info) {
        struct virtio_gpu_ctrl_hdr cmd = {};
        cmd.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

        struct virtio_gpu_resp_display_info resp;
        if (virtio_gpu_submit(d, &cmd, sizeof(cmd), &resp, sizeof(resp), 2000) < 0 ||
            resp.hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
            printk("virtio-gpu: get_display_info failed (resp 0x%x)\n", resp.hdr.type);
            virtio_gpu_cleanup(d);
            return false;
        }

        uint32_t w = 0, h = 0;
        for (uint32_t i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
            if (resp.pmodes[i].enabled && resp.pmodes[i].r.width &&
                resp.pmodes[i].r.height) {
                w = resp.pmodes[i].r.width;
                h = resp.pmodes[i].r.height;
                break;
            }
        }
        if (w == 0 || h == 0 || w > 8192 || h > 8192) {
            w = 1024;       /* no (sane) enabled scanout: use a default */
            h = 768;
        }
        d->scanout_width = w;
        d->scanout_height = h;
        d->has_display_info = 1;
    }
    cur_.width = d->scanout_width;
    cur_.height = d->scanout_height;
    cur_.pitch = cur_.width * 4;                 /* 32bpp X8R8G8B8 */
    cur_.format = PixelFormat::X8R8G8B8;
    printk("virtio-gpu: display %ux%u\n", cur_.width, cur_.height);

    /* Honour a specific mode request; zeros mean "keep what the host has". */
    if ((mode.width != 0 && mode.width != cur_.width) ||
        (mode.height != 0 && mode.height != cur_.height)) {
        printk("virtio-gpu: requested %ux%u not available\n", mode.width, mode.height);
        virtio_gpu_cleanup(d);
        return false;
    }

    if (!create_framebuffer_resource()) {
        virtio_gpu_cleanup(d);
        return false;
    }
    /* Before SET_SCANOUT: the host may sample the resource as soon as it
       becomes the scanout. Copy first so that sample is the live frame. */
    copy_console_pixels();
    if (!setup_scanout()) {
        virtio_gpu_cleanup(d);       /* reset first: host must stop reading fb */
        release_framebuffer();
        return false;
    }

    /* ready_ gates present_flush. Push the seeded frame now; the console
       still draws the GOP back-buffer until select_best() publishes us. */
    ready_ = true;
    if (!present_flush(GfxRect{}))
        printk("virtio-gpu: initial scanout flush failed\n");

    /* 3D: only advertise it if a virgl context AND a capset really exist. */
    if (d->has_virgl_3d && ctx_id_ == 0) {
        uint32_t ctx = virtio_gpu_alloc_context_id();
        bool ok = false;
        if (virtio_gpu_create_context(ctx, 0, "axiome") == 0) {
            if (d->num_capsets > 0) {
                struct virtio_gpu_get_capset_info ci = {};
                ci.hdr.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO;
                ci.capset_index = 0;
                struct virtio_gpu_resp_capset_info cr;
                if (virtio_gpu_submit(d, &ci, sizeof(ci), &cr, sizeof(cr), 2000) == 0 &&
                    cr.capset_id >= 1 && cr.capset_id <= 63 &&
                    cr.capset_max_size > 0 && cr.capset_max_size <= 8u * 1024 * 1024) {
                    printk("virtio-gpu: capset0 id=%u ver=%u size=%u\n",
                           cr.capset_id, cr.capset_max_version, cr.capset_max_size);
                    ok = true;
                } else {
                    printk("virtio-gpu: capset0 query failed/bogus\n");
                }
            } else {
                printk("virtio-gpu: host exposes virgl but 0 capsets\n");
            }
            if (!ok)
                virtio_gpu_destroy_context(ctx);
        } else {
            printk("virtio-gpu: ctx_create failed\n");
        }
        if (ok) {
            ctx_id_ = ctx;
            printk("virtio-gpu: 3D context %u ready\n", ctx);
        } else {
            d->has_virgl_3d = 0;     /* caps() must not claim 3D */
        }
    }

    ready_ = true;
    printk("gfx: virtio-gpu online %ux%u pitch=%u 3D=%s\n",
           cur_.width, cur_.height, cur_.pitch, d->has_virgl_3d ? "yes" : "no");
    return true;
}

GfxCaps VirtioGpuDisplay::caps() const
{
    GfxCaps c;
    /* Everything here is CPU-driven: clear/fill/blit loop over the guest
       framebuffer and present() does TRANSFER+FLUSH. Do not claim hardware
       acceleration the backend does not implement. */
    c.has_hw_fill = 0;
    c.has_hw_blit = 0;
    c.has_hw_flip = 0;
    c.has_3d = virtio_gpu_dev()->has_virgl_3d ? 1 : 0;
    c.has_alpha = 1;       /* CPU alpha blending done by the manager */
    c.has_compositor = 1;
    c.has_shadows = 0;
    c.has_vsync = 0;
    c.has_hw_cursor = 0;
    c.has_scale = 0;
    c.has_yuv = 0;
    c.has_shaders = 0;
    c.has_blur = 0;
    c.bits = 0;
    gfx_caps_sync_bits(c);
    return c;
}

void VirtioGpuDisplay::clear(uint32_t rgb)
{
    if (!ready_ || !fb_)
        return;
    uint32_t native = canonical_to_native(rgb, cur_.format);
    uint32_t *dst = (uint32_t *)fb_;
    uint32_t n = cur_.height * (cur_.pitch / 4);
    for (uint32_t i = 0; i < n; i++)
        dst[i] = native;
    /* No flush here: the compositor calls present() once per frame. */
}

void VirtioGpuDisplay::fill_rect(const GfxRect &r, uint32_t rgb)
{
    if (!ready_ || !fb_)
        return;
    GfxRect c = r;
    if (!clip_rect(c, cur_.width, cur_.height))
        return;
    uint32_t native = canonical_to_native(rgb, cur_.format);
    uint8_t *base = (uint8_t *)fb_;
    for (uint32_t y = c.y; y < c.y + c.h; y++) {
        uint32_t *row = (uint32_t *)(base + (size_t)y * cur_.pitch + (size_t)c.x * 4);
        for (uint32_t x = 0; x < c.w; x++)
            row[x] = native;
    }
}

void VirtioGpuDisplay::blit(const GfxRect &dst, const uint32_t *src,
                            uint32_t src_pitch_px)
{
    if (!ready_ || !fb_ || !src)
        return;
    GfxRect c = dst;
    if (!clip_rect(c, cur_.width, cur_.height))
        return;
    uint8_t *base = (uint8_t *)fb_;
    uint32_t sx = c.x - dst.x;
    uint32_t sy = c.y - dst.y;
    for (uint32_t y = 0; y < c.h; y++) {
        const uint32_t *srow = src + (size_t)(sy + y) * src_pitch_px + sx;
        uint32_t *drow = (uint32_t *)(base + (size_t)(c.y + y) * cur_.pitch +
                                      (size_t)c.x * 4);
        for (uint32_t x = 0; x < c.w; x++)
            drow[x] = canonical_to_native(srow[x], cur_.format);
    }
}

bool VirtioGpuDisplay::flip(uint32_t resource_id)
{
    if (!ready_ || resource_id == 0)     /* id 0 would disable the scanout */
        return false;
    struct virtio_gpu_set_scanout cmd = {};
    cmd.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    cmd.r.x = 0;
    cmd.r.y = 0;
    cmd.r.width = cur_.width;
    cmd.r.height = cur_.height;
    cmd.scanout_id = 0;
    cmd.resource_id = resource_id;

    struct virtio_gpu_ctrl_hdr resp;
    if (virtio_gpu_submit(virtio_gpu_dev(), &cmd, sizeof(cmd),
                          &resp, sizeof(resp), 1000) < 0) {
        printk("virtio-gpu: flip to resource %u failed\n", resource_id);
        return false;
    }
    /* fb_resource_id_ (CPU-drawn, has backing) stays put so flipping back to it
       and later present() calls keep working. */
    scanout_resource_id_ = resource_id;
    return true;
}

bool VirtioGpuDisplay::present_rect(const GfxRect &r)
{
    return present_flush(r);
}

void VirtioGpuDisplay::present()
{
    present_flush(GfxRect{});   /* full frame */
}

} /* namespace gfx */
