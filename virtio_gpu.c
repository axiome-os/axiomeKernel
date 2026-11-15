#include "virtio_gpu.h"
#include "pci.h"
#include "io.h"
#include "pmm.h"
#include "vmm.h"
#include "slab.h"
#include "printk.h"
#include "string.h"
#include "spinlock.h"
#include "hal/cshim.h"
#include <stddef.h>

/* ---- tunables ---- */
#define VG_CMD_PAGES      16u
#define VG_RESP_PAGES     4u
#define VG_CMD_MAX        (VG_CMD_PAGES * PAGE_SIZE)
#define VG_RESP_MAX       (VG_RESP_PAGES * PAGE_SIZE)
#define VG_QUEUE_ENTRIES  64u

/* Rings and bounce buffers: write-back. x86 DMA is cache-coherent and the
   kernel's identity map of the same frames is WB, so a WB alias avoids
   mixed-attribute aliasing. Add MMU_UNCACHED here if you prefer the old way;
   the explicit fences below make either work. */
#define VG_DMA_PTE        (MMU_WRITE)
#define VG_MMIO_PTE       (MMU_WRITE | MMU_UNCACHED)

/* ---- virtio PCI capability types ---- */
#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_DEVICE_CFG 4

#define VRING_DESC_F_NEXT   1
#define VRING_DESC_F_WRITE  2
#define VRING_AVAIL_F_NO_INTERRUPT 1

/* ---- split-ring structures (private) ---- */
struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
};

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
};

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[];
};

/* ---- virtio_pci_common_cfg (spec 4.1.4.3) ---- */
struct vg_common_cfg {
    volatile uint32_t device_feature_select;  /* 0x00 */
    volatile uint32_t device_feature;         /* 0x04 */
    volatile uint32_t driver_feature_select;  /* 0x08 */
    volatile uint32_t driver_feature;         /* 0x0C */
    volatile uint16_t msix_config;            /* 0x10 */
    volatile uint16_t num_queues;             /* 0x12 */
    volatile uint8_t  device_status;          /* 0x14 */
    volatile uint8_t  config_generation;      /* 0x15 */
    volatile uint16_t queue_select;           /* 0x16 */
    volatile uint16_t queue_size;             /* 0x18 */
    volatile uint16_t queue_msix_vector;      /* 0x1A */
    volatile uint16_t queue_enable;           /* 0x1C */
    volatile uint16_t queue_notify_off;       /* 0x1E */
    volatile uint32_t queue_desc_lo;          /* 0x20 */
    volatile uint32_t queue_desc_hi;          /* 0x24 */
    volatile uint32_t queue_avail_lo;         /* 0x28 */
    volatile uint32_t queue_avail_hi;         /* 0x2C */
    volatile uint32_t queue_used_lo;          /* 0x30 */
    volatile uint32_t queue_used_hi;          /* 0x34 */
};

#define VG_COMMON_CFG_SIZE 0x38u
#define CC(v) ((volatile struct vg_common_cfg *)(v)->common_cfg)

_Static_assert(offsetof(struct vg_common_cfg, device_status) == 0x14, "cfg layout");
_Static_assert(offsetof(struct vg_common_cfg, queue_enable) == 0x1C, "cfg layout");
_Static_assert(offsetof(struct vg_common_cfg, queue_desc_lo) == 0x20, "cfg layout");
_Static_assert(offsetof(struct vg_common_cfg, queue_used_hi) == 0x34, "cfg layout");
_Static_assert(sizeof(struct virtio_gpu_ctrl_hdr) == 24, "ctrl hdr size");
_Static_assert(sizeof(struct virtio_gpu_resp_display_info) == 24 + 16 * 24, "display info size");

static struct virtio_gpu_device g_vgpu;
static spinlock_t g_vg_lock = SPINLOCK_INIT;
static uint64_t g_tsc_per_ms;
static unsigned g_vg_errs;

/* ---------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------- */

static inline void vg_mb(void) { __asm__ volatile("mfence" ::: "memory"); }
static inline void vg_cb(void) { __asm__ volatile("" ::: "memory"); }

static inline uint64_t vg_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* One-time TSC calibration against PIT channel 2 (same method as apic.c).
   Needed because timeouts must work with IF=0 (clock_mono_ns() only advances
   from the timer ISR). */
static void vg_tsc_init(void)
{
    if (g_tsc_per_ms)
        return;

    const uint16_t count = 23863;           /* 20 ms at 1193182 Hz */
    uint8_t ctl = inb(0x61);
    outb(0x61, (uint8_t)(ctl & 0xFE));
    outb(0x61, (uint8_t)((ctl & 0xFC) | 0x01));
    outb(0x43, 0xB0);
    outb(0x42, (uint8_t)(count & 0xFF));
    outb(0x42, (uint8_t)(count >> 8));

    uint64_t t0 = vg_rdtsc();
    uint32_t guard = 0;
    while (!(inb(0x61) & 0x20) && ++guard < 2000000u)
        hal_cpu_pause();
    uint64_t t1 = vg_rdtsc();
    outb(0x61, ctl);

    uint64_t per_ms = (t1 - t0) / 20;
    if (guard >= 2000000u || per_ms < 100000ULL || per_ms > 20000000ULL)
        per_ms = 2000000ULL;                /* fallback: assume ~2 GHz */
    g_tsc_per_ms = per_ms;
}

static inline uint64_t vg_deadline(uint32_t ms)
{
    return vg_rdtsc() + (uint64_t)ms * g_tsc_per_ms;
}

static inline int vg_expired(uint64_t deadline)
{
    return (int64_t)(vg_rdtsc() - deadline) > 0;
}

/* ---------------------------------------------------------------------------
 * DMA memory
 * ------------------------------------------------------------------------- */

static int vg_dma_alloc(struct virtio_gpu_dma *d, uint32_t pages)
{
    /* Prefer <4 GiB (top-down); any address works for a 64-bit virtio device,
       so fall back to anywhere. The PMM already reserves the low 1 MiB. */
    void *phys = pmm_alloc_frames_high(pages, 0x100000000ULL);
    if (!phys)
        phys = pmm_alloc_frames(pages);
    if (!phys)
        return -1;

    void *virt = vmm_mmap_phys((uint64_t)(uintptr_t)phys, pages, VG_DMA_PTE);
    if (!virt) {
        pmm_free_frames(phys, pages);
        return -1;
    }
    d->virt = virt;
    d->phys = (uint64_t)(uintptr_t)phys;
    d->pages = pages;
    memset(virt, 0, (size_t)pages * PAGE_SIZE);
    return 0;
}

static void vg_dma_free(struct virtio_gpu_dma *d)
{
    if (!d->virt)
        return;
    /* Unmap the VIRTUAL pages (every one), then return the physical frames. */
    for (uint32_t i = 0; i < d->pages; i++)
        vmm_unmap_page((uint64_t)(uintptr_t)d->virt + (uint64_t)i * PAGE_SIZE);
    pmm_free_frames((void *)(uintptr_t)d->phys, d->pages);
    memset(d, 0, sizeof(*d));
}

/* ---------------------------------------------------------------------------
 * PCI capability parsing / BAR windows
 * ------------------------------------------------------------------------- */

static int vg_find_caps(struct virtio_gpu_device *v)
{
    const struct pci_device *p = v->pdev;
    uint8_t b = p->bus, d = p->dev, f = p->func;

    if (!((pci_read32(b, d, f, 0x04) >> 16) & 0x10))   /* status: cap list */
        return -1;

    unsigned ptr = pci_read8(b, d, f, 0x34) & 0xFCu;
    for (int guard = 0; ptr && guard < 48; guard++) {
        unsigned id   = pci_read8(b, d, f, (uint8_t)ptr);
        unsigned next = pci_read8(b, d, f, (uint8_t)(ptr + 1)) & 0xFCu;

        if (id == 0x09) {                               /* vendor specific */
            unsigned len  = pci_read8(b, d, f, (uint8_t)(ptr + 2));
            unsigned type = pci_read8(b, d, f, (uint8_t)(ptr + 3));
            if (len >= 16 && ptr + len <= 0x100) {
                struct virtio_gpu_cap c;
                c.present = 1;
                c.bar     = pci_read8(b, d, f, (uint8_t)(ptr + 4));
                c.offset  = pci_read32(b, d, f, (uint8_t)(ptr + 8));
                c.length  = pci_read32(b, d, f, (uint8_t)(ptr + 12));
                if (c.bar < 6) {
                    if (type == VIRTIO_PCI_CAP_COMMON_CFG && !v->cap_common.present)
                        v->cap_common = c;
                    else if (type == VIRTIO_PCI_CAP_NOTIFY_CFG && len >= 20 &&
                             !v->cap_notify.present) {
                        v->cap_notify = c;
                        v->notify_mult = pci_read32(b, d, f, (uint8_t)(ptr + 16));
                    } else if (type == VIRTIO_PCI_CAP_DEVICE_CFG &&
                               !v->cap_device.present)
                        v->cap_device = c;
                }
            }
        }
        ptr = next;
    }
    return (v->cap_common.present && v->cap_notify.present) ? 0 : -1;
}

/* Map exactly [offset, offset+length) of BAR `c->bar`, uncached. */
static volatile uint8_t *vg_map_region(const struct pci_device *p,
                                       const struct virtio_gpu_cap *c)
{
    if (!c->present || c->length == 0 || c->bar >= 6)
        return 0;
    if (p->bar[c->bar] & 1)                 /* I/O BAR: not valid for modern */
        return 0;
    uint64_t base = pci_bar_addr(p, c->bar);
    if (!base)
        return 0;

    uint64_t phys  = base + c->offset;
    uint64_t poff  = phys & (PAGE_SIZE - 1);
    size_t   pages = (size_t)((poff + c->length + PAGE_SIZE - 1) >> PAGE_SHIFT);
    void *v = vmm_mmap_phys(phys - poff, pages, VG_MMIO_PTE);
    return v ? (volatile uint8_t *)v + poff : 0;
}

/* Mapped once and kept for the life of the kernel (vmm_mmap_phys never reuses
   virtual addresses, so re-mapping on every re-init would leak VA space). */
static int vg_map_regs(struct virtio_gpu_device *v)
{
    if (v->regs_mapped)
        return 0;

    if (v->cap_common.length < VG_COMMON_CFG_SIZE || v->cap_notify.length < 2) {
        printk("virtio-gpu: capability windows too small\n");
        return -1;
    }
    v->common_cfg = vg_map_region(v->pdev, &v->cap_common);
    v->notify_base = vg_map_region(v->pdev, &v->cap_notify);
    if (v->cap_device.present)
        v->device_cfg = vg_map_region(v->pdev, &v->cap_device);
    if (!v->common_cfg || !v->notify_base) {
        printk("virtio-gpu: failed to map BAR windows\n");
        v->common_cfg = 0;
        v->notify_base = 0;
        v->device_cfg = 0;
        return -1;
    }
    v->regs_mapped = 1;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Device reset / queue setup
 * ------------------------------------------------------------------------- */

/* Spec 4.1.4.3.2: write 0, then read until the device reports 0. */
static int vg_reset(struct virtio_gpu_device *v)
{
    volatile struct vg_common_cfg *cc = CC(v);
    if (!cc)
        return -1;
    cc->device_status = 0;
    vg_mb();
    uint64_t deadline = vg_deadline(200);
    while (cc->device_status != 0) {
        if (vg_expired(deadline))
            return -1;
        hal_cpu_pause();
    }
    return 0;
}

static int vg_vring_setup(struct virtio_gpu_device *v,
                          struct virtio_gpu_vring *r, uint16_t qidx)
{
    volatile struct vg_common_cfg *cc = CC(v);

    cc->queue_select = qidx;
    vg_mb();
    uint32_t max = cc->queue_size;
    if (max == 0) {
        printk("virtio-gpu: queue %u not available\n", (unsigned)qidx);
        return -1;
    }
    uint32_t n = VG_QUEUE_ENTRIES < max ? VG_QUEUE_ENTRIES : max;   /* both pow2 */

    size_t desc_sz   = (size_t)n * sizeof(struct vring_desc);
    size_t avail_off = desc_sz;
    size_t avail_sz  = 6 + 2 * (size_t)n;
    size_t used_off  = (avail_off + avail_sz + 3) & ~(size_t)3;
    size_t used_sz   = 6 + 8 * (size_t)n;
    size_t total     = used_off + used_sz;
    uint32_t pages   = (uint32_t)((total + PAGE_SIZE - 1) >> PAGE_SHIFT);

    memset(r, 0, sizeof(*r));
    if (vg_dma_alloc(&r->mem, pages) != 0) {
        printk("virtio-gpu: vring alloc failed\n");
        return -1;
    }
    uint8_t *base = (uint8_t *)r->mem.virt;
    r->idx = qidx;
    r->entries = (uint16_t)n;
    r->desc  = (struct vring_desc *)base;
    r->avail = (struct vring_avail *)(base + avail_off);
    r->used  = (struct vring_used *)(base + used_off);
    r->avail->flags = VRING_AVAIL_F_NO_INTERRUPT;   /* we poll */

    uint64_t pd = r->mem.phys;
    uint64_t pa = pd + avail_off;
    uint64_t pu = pd + used_off;

    cc->queue_size = (uint16_t)n;
    cc->queue_msix_vector = 0xFFFF;                 /* VIRTIO_MSI_NO_VECTOR */
    cc->queue_desc_lo  = (uint32_t)pd;  cc->queue_desc_hi  = (uint32_t)(pd >> 32);
    cc->queue_avail_lo = (uint32_t)pa;  cc->queue_avail_hi = (uint32_t)(pa >> 32);
    cc->queue_used_lo  = (uint32_t)pu;  cc->queue_used_hi  = (uint32_t)(pu >> 32);
    vg_mb();

    r->notify_off = cc->queue_notify_off;
    if ((uint64_t)r->notify_off * v->notify_mult + 2 > v->cap_notify.length) {
        printk("virtio-gpu: notify offset out of range\n");
        vg_dma_free(&r->mem);
        return -1;
    }

    cc->queue_enable = 1;
    vg_mb();
    if (cc->queue_enable != 1) {
        printk("virtio-gpu: queue %u failed to enable\n", (unsigned)qidx);
        vg_dma_free(&r->mem);
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Probe / init / cleanup
 * ------------------------------------------------------------------------- */

int virtio_gpu_probe(void)
{
    struct virtio_gpu_device *v = &g_vgpu;
    if (v->detected)
        return 1;

    for (struct pci_device *p = pci_first(); p; p = p->next) {
        if (p->vendor != VIRTIO_GPU_VENDOR || p->device != VIRTIO_GPU_DEVICE)
            continue;

        memset(v, 0, sizeof(*v));
        v->pdev = p;
        v->bus = p->bus;
        v->dev = p->dev;
        v->func = p->func;
        v->irq = p->irq;
        if (vg_find_caps(v) != 0) {
            printk("virtio-gpu: %02x:%02x.%u has no modern virtio caps (legacy "
                   "device unsupported)\n", p->bus, p->dev, p->func);
            memset(v, 0, sizeof(*v));
            return 0;
        }
        v->fence_id = 1;
        v->next_resource_id = 1;
        v->next_context_id = 1;
        v->detected = 1;
        printk("virtio-gpu: detected at %02x:%02x.%u (common bar%u+0x%x, notify "
               "bar%u+0x%x x%u)\n",
               p->bus, p->dev, p->func,
               (unsigned)v->cap_common.bar, v->cap_common.offset,
               (unsigned)v->cap_notify.bar, v->cap_notify.offset, v->notify_mult);
        return 1;
    }
    return 0;
}

void virtio_gpu_cleanup(struct virtio_gpu_device *dev)
{
    struct virtio_gpu_device *v = dev ? dev : &g_vgpu;
    unsigned long fl = spin_lock_irq(&g_vg_lock);

    int stopped = 1;
    if (v->common_cfg)
        stopped = (vg_reset(v) == 0);   /* device must stop DMA before we free */

    if (stopped) {
        vg_dma_free(&v->control.mem);
        vg_dma_free(&v->cmd);
        vg_dma_free(&v->resp);
    } else {
        printk("virtio-gpu: device did not reset; leaking DMA buffers\n");
    }
    memset(&v->control, 0, sizeof(v->control));
    memset(&v->cmd, 0, sizeof(v->cmd));
    memset(&v->resp, 0, sizeof(v->resp));

    v->initialized = 0;
    v->has_display_info = 0;
    v->fb_resource_id = 0;
    v->has_virgl_3d = v->has_edid = v->has_resource_blob = v->has_context_init = 0;
    v->device_features = v->driver_features = 0;

    spin_unlock_irq(&g_vg_lock, fl);
}

int virtio_gpu_init(struct virtio_gpu_device *dev)
{
    struct virtio_gpu_device *v = dev ? dev : &g_vgpu;
    volatile struct vg_common_cfg *cc;
    struct pci_device *p;
    uint32_t pcicmd, lo, hi;
    uint64_t feat, wanted;
    uint8_t status;

    if (!v->detected)
        return -1;
    if (v->initialized)
        return 0;

    vg_tsc_init();
    if (vg_map_regs(v) != 0)
        return -1;
    cc = CC(v);
    p = v->pdev;

    /* Memory space + bus master + INTx disable (we never take interrupts).
       Mask the upper half: PCI status bits there are write-1-to-clear. */
    pcicmd = pci_read32(p->bus, p->dev, p->func, 0x04);
    pci_write32(p->bus, p->dev, p->func, 0x04,
                (pcicmd & 0xFFFFu) | 0x0006u | 0x0400u);

    if (vg_reset(v) != 0) {
        printk("virtio-gpu: device did not reset\n");
        return -1;
    }
    status = VIRTIO_STATUS_ACK;
    cc->device_status = status;
    status |= VIRTIO_STATUS_DRIVER;
    cc->device_status = status;

    /* Features. */
    cc->device_feature_select = 0;
    vg_mb();
    lo = cc->device_feature;
    cc->device_feature_select = 1;
    vg_mb();
    hi = cc->device_feature;
    feat = ((uint64_t)hi << 32) | lo;
    v->device_features = feat;

    if (!(feat & (1ULL << VIRTIO_F_VERSION_1))) {
        printk("virtio-gpu: device is legacy-only (features 0x%lx)\n",
               (unsigned long)feat);
        goto fail;
    }
    wanted = 1ULL << VIRTIO_F_VERSION_1;
    if (feat & (1ULL << VIRTIO_GPU_F_VIRGL))
        wanted |= 1ULL << VIRTIO_GPU_F_VIRGL;       /* only what we use */

    cc->driver_feature_select = 0;
    cc->driver_feature = (uint32_t)wanted;
    cc->driver_feature_select = 1;
    cc->driver_feature = (uint32_t)(wanted >> 32);
    v->driver_features = wanted;

    status |= VIRTIO_STATUS_FEATURES_OK;
    cc->device_status = status;
    vg_mb();
    if (!(cc->device_status & VIRTIO_STATUS_FEATURES_OK)) {
        printk("virtio-gpu: FEATURES_OK rejected (dev 0x%lx drv 0x%lx)\n",
               (unsigned long)feat, (unsigned long)wanted);
        goto fail;
    }

    v->has_virgl_3d = (wanted & (1ULL << VIRTIO_GPU_F_VIRGL)) != 0;
    v->has_edid = (wanted & (1ULL << VIRTIO_GPU_F_EDID)) != 0;
    v->has_resource_blob = (wanted & (1ULL << VIRTIO_GPU_F_RESOURCE_BLOB)) != 0;
    v->has_context_init = (wanted & (1ULL << VIRTIO_GPU_F_CONTEXT_INIT)) != 0;

    /* Device config: events_read(0) events_clear(4) num_scanouts(8) num_capsets(12). */
    v->num_scanouts = 1;
    v->num_capsets = 0;
    if (v->device_cfg && v->cap_device.length >= sizeof(struct virtio_gpu_config)) {
        v->num_scanouts = *(volatile uint32_t *)(v->device_cfg + 8);
        v->num_capsets  = *(volatile uint32_t *)(v->device_cfg + 12);
        if (v->num_scanouts == 0)
            v->num_scanouts = 1;
    }

    if (vg_dma_alloc(&v->cmd, VG_CMD_PAGES) != 0 ||
        vg_dma_alloc(&v->resp, VG_RESP_PAGES) != 0) {
        printk("virtio-gpu: failed to allocate command buffers\n");
        goto fail;
    }
    if (vg_vring_setup(v, &v->control, 0) != 0)
        goto fail;

    status |= VIRTIO_STATUS_DRIVER_OK;
    cc->device_status = status;
    vg_mb();
    if (cc->device_status & (VIRTIO_STATUS_DEVICE_NEEDS_RESET | VIRTIO_STATUS_FAILED)) {
        printk("virtio-gpu: device failed after DRIVER_OK (status 0x%x)\n",
               (unsigned)cc->device_status);
        goto fail;
    }

    v->initialized = 1;
    printk("virtio-gpu: ready (features 0x%lx, qsize %u, scanouts %u, capsets %u, virgl %d)\n",
           (unsigned long)wanted, (unsigned)v->control.entries,
           v->num_scanouts, v->num_capsets, v->has_virgl_3d);
    return 0;

fail:
    cc->device_status = VIRTIO_STATUS_FAILED;
    virtio_gpu_cleanup(v);
    return -1;
}

struct virtio_gpu_device *virtio_gpu_dev(void)
{
    return &g_vgpu;
}

/* ---------------------------------------------------------------------------
 * Command submission
 * ------------------------------------------------------------------------- */

static int vg_submit_locked(struct virtio_gpu_device *v,
                            const void *cmd, size_t cmd_len,
                            void *resp, size_t resp_len, uint32_t rlen,
                            int timeout_ms)
{
    struct virtio_gpu_vring *r = &v->control;
    volatile struct vring_used *used = r->used;
    volatile struct vring_avail *avail = r->avail;
    struct vring_desc *d = r->desc;
    uint32_t ctype;

    if (!v->initialized || !d)
        return -1;

    /* A previous command timed out. If the device has finished it since, resync;
       otherwise fail fast: descriptors/buffers are still owned by the device. */
    if (r->outstanding) {
        uint16_t u = used->idx;
        if (u != r->avail_idx)
            return -1;
        r->used_idx = u;
        r->outstanding = 0;
        printk("virtio-gpu: late completion recovered\n");
    }

    ctype = ((const struct virtio_gpu_ctrl_hdr *)cmd)->type;

    memcpy(v->cmd.virt, cmd, cmd_len);
    memset(v->resp.virt, 0, rlen);

    d[0].addr = v->cmd.phys;
    d[0].len = (uint32_t)cmd_len;
    d[0].flags = VRING_DESC_F_NEXT;
    d[0].next = 1;
    d[1].addr = v->resp.phys;
    d[1].len = rlen;
    d[1].flags = VRING_DESC_F_WRITE;
    d[1].next = 0;

    avail->ring[r->avail_idx % r->entries] = 0;     /* head = desc 0 */
    vg_cb();
    r->avail_idx++;
    avail->idx = r->avail_idx;
    vg_mb();                                         /* publish before notify */

    *(volatile uint16_t *)(v->notify_base +
                           (uint64_t)r->notify_off * v->notify_mult) = r->idx;

    uint64_t deadline = vg_deadline(timeout_ms > 0 ? (uint32_t)timeout_ms : 2000u);
    while (used->idx == r->used_idx) {
        hal_cpu_pause();
        if (vg_expired(deadline)) {
            r->outstanding = 1;
            printk("virtio-gpu: timeout (cmd 0x%x); queue parked until device answers\n",
                   ctype);
            return -1;
        }
    }
    vg_cb();
    r->used_idx = used->idx;

    uint32_t rtype = ((struct virtio_gpu_ctrl_hdr *)v->resp.virt)->type;
    if (resp && resp_len)
        memcpy(resp, v->resp.virt, resp_len < rlen ? resp_len : rlen);

    if (rtype < VIRTIO_GPU_RESP_OK_NODATA || rtype >= VIRTIO_GPU_RESP_ERR_UNSPEC) {
        if (g_vg_errs++ < 16)
            printk("virtio-gpu: cmd 0x%x -> resp 0x%x\n", ctype, rtype);
        return -1;
    }
    return 0;
}

int virtio_gpu_submit(struct virtio_gpu_device *vgpu,
                      void *cmd, size_t cmd_len,
                      void *resp, size_t resp_len,
                      int timeout_ms)
{
    struct virtio_gpu_device *v = vgpu ? vgpu : &g_vgpu;

    if (resp && resp_len)
        memset(resp, 0, resp_len);          /* callers may always inspect it */
    if (!cmd || cmd_len < sizeof(struct virtio_gpu_ctrl_hdr) || cmd_len > VG_CMD_MAX)
        return -1;
    if (resp_len > VG_RESP_MAX)
        return -1;

    uint32_t rlen = (resp && resp_len >= sizeof(struct virtio_gpu_ctrl_hdr))
                        ? (uint32_t)resp_len
                        : (uint32_t)sizeof(struct virtio_gpu_ctrl_hdr);

    unsigned long fl = spin_lock_irq(&g_vg_lock);
    int rc = vg_submit_locked(v, cmd, cmd_len, resp, resp_len, rlen, timeout_ms);
    spin_unlock_irq(&g_vg_lock, fl);
    return rc;
}

/* ---------------------------------------------------------------------------
 * ID allocation and 3D helpers
 * ------------------------------------------------------------------------- */

uint32_t virtio_gpu_alloc_resource_id(void)
{
    return __sync_fetch_and_add(&g_vgpu.next_resource_id, 1u);
}

uint32_t virtio_gpu_alloc_context_id(void)
{
    return __sync_fetch_and_add(&g_vgpu.next_context_id, 1u);
}

int virtio_gpu_create_context(uint32_t ctx_id, uint32_t context_init, const char *name)
{
    struct virtio_gpu_ctx_create cmd;
    struct virtio_gpu_ctrl_hdr resp;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
    cmd.hdr.ctx_id = ctx_id;
    cmd.nlen = name ? (uint32_t)strlen(name) : 0;
    if (cmd.nlen > 63)
        cmd.nlen = 63;
    cmd.context_init = context_init;
    if (name && cmd.nlen)
        memcpy(cmd.debug_name, name, cmd.nlen);
    return virtio_gpu_submit(&g_vgpu, &cmd, sizeof(cmd), &resp, sizeof(resp), 2000);
}

int virtio_gpu_destroy_context(uint32_t ctx_id)
{
    struct virtio_gpu_ctx_destroy cmd;
    struct virtio_gpu_ctrl_hdr resp;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_CTX_DESTROY;
    cmd.hdr.ctx_id = ctx_id;
    return virtio_gpu_submit(&g_vgpu, &cmd, sizeof(cmd), &resp, sizeof(resp), 1000);
}

int virtio_gpu_resource_create_3d(uint32_t resource_id, uint32_t width, uint32_t height,
                                  uint32_t target, uint32_t format, uint32_t bind)
{
    struct virtio_gpu_resource_create_3d cmd;
    struct virtio_gpu_ctrl_hdr resp;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    cmd.resource_id = resource_id;
    cmd.target = target ? target : 2;                /* PIPE_TEXTURE_2D */
    cmd.format = format ? format : VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    cmd.bind = bind;
    cmd.width = width;
    cmd.height = height;
    cmd.depth = 1;
    cmd.array_size = 1;
    return virtio_gpu_submit(&g_vgpu, &cmd, sizeof(cmd), &resp, sizeof(resp), 2000);
}

int virtio_gpu_submit_3d(uint32_t ctx_id, void *cmd_buf, uint32_t cmd_size)
{
    size_t total = sizeof(struct virtio_gpu_cmd_submit) + (size_t)cmd_size;
    if ((!cmd_buf && cmd_size) || total > VG_CMD_MAX)
        return -1;

    uint8_t *buf = (uint8_t *)kmalloc(total);
    if (!buf)
        return -1;
    struct virtio_gpu_cmd_submit *hdr = (struct virtio_gpu_cmd_submit *)buf;
    memset(hdr, 0, sizeof(*hdr));
    hdr->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    hdr->hdr.ctx_id = ctx_id;
    hdr->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    hdr->hdr.fence_id = __sync_fetch_and_add(&g_vgpu.fence_id, 1ULL);
    hdr->size = cmd_size;
    if (cmd_size)
        memcpy(buf + sizeof(*hdr), cmd_buf, cmd_size);

    struct virtio_gpu_ctrl_hdr resp;
    int r = virtio_gpu_submit(&g_vgpu, buf, total, &resp, sizeof(resp), 2000);
    kfree(buf);
    return r;
}

int virtio_gpu_transfer_to_host_3d(uint32_t ctx_id, uint32_t resource_id,
                                   uint64_t offset, struct virtio_gpu_box *box,
                                   uint32_t level, uint32_t stride, uint32_t layer_stride)
{
    struct virtio_gpu_transfer_host_3d cmd;
    struct virtio_gpu_ctrl_hdr resp;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D;
    cmd.hdr.ctx_id = ctx_id;
    cmd.resource_id = resource_id;
    if (box)
        cmd.box = *box;
    cmd.offset = offset;
    cmd.level = level;
    cmd.stride = stride;
    cmd.layer_stride = layer_stride;
    return virtio_gpu_submit(&g_vgpu, &cmd, sizeof(cmd), &resp, sizeof(resp), 1000);
}
