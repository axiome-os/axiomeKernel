/*
 * virtio-net driver — OASIS Virtio 1.0 through 1.2, PCI transport, split rings.
 *
 * PCI discovery (transitional + modern), legacy BAR0 I/O interface and modern
 * capability-based interface, split virtqueues (descriptor table, available
 * ring, used ring, indirect descriptors, event-index suppression), virtio-net
 * header, Tx/Rx data paths with mergeable receive buffers, control virtqueue,
 * MSI-X / INTx interrupts, and initialization/teardown.
 *
 * Adapted to axiomeOS primitives: pci.h (PCI config), io.h (port I/O),
 * hal/cshim.h (MMIO map, memory barriers), pmm.h/vmm.h (coherent DMA),
 * netdev/net_buf (mbuf network stack), ioapic.h (legacy IRQ routing).
 *
 * x86_64 is little-endian: modern virtio is little-endian on the wire and the
 * legacy interface uses guest-native endianness, so both read natively here.
 * Big-endian hosts would need byte swaps in the ring/header accessors; they
 * are explicitly unsupported at present (see VIO_BIG_ENDIAN_HOST).
 */

#include "virtio_net.h"

#include "pci.h"
#include "io.h"
#include "hal/cshim.h"
#include "printk.h"
#include "string.h"
#include "pmm.h"
#include "vmm.h"
#include "netdev.h"
#include "net_buf.h"
#include "net_util.h"
#include "ioapic.h"
#include "spinlock.h"
#include "slab.h"
#include "dhcp.h"
#include "driver.h"

/* ---------------------------------------------------------------- *
 * Configuration constants                                            *
 * ---------------------------------------------------------------- */

#define VNET_QUEUE_SIZE       256u   /* ring size for rx1 / tx1 */
#define VNET_CTRL_QUEUE_SIZE   64u   /* ring size for control queue */
#define VNET_MAX_FRAGS         17u   /* descriptors per Tx chain (incl. header) */
#define VNET_RX_BUF_MRG       2048u  /* fixed Rx buffer size with MRG_RXBUF */
#define IND_MAX                18u   /* indirect-table entries per head desc */

#define VNET_MSI_VECTOR_BASE  0x30u  /* IDT vectors for MSI-X (cfg, rx, tx) */

/* ---------------------------------------------------------------- *
 * Endianness / barriers                                              *
 * ---------------------------------------------------------------- */

#define VIO_BIG_ENDIAN_HOST 0

#if VIO_BIG_ENDIAN_HOST
#error "big-endian host not supported: add le16/le32/le64 byte swaps"
#endif

/* On a little-endian host the wire format is native; these are identity so the
   code reads identically on every host and documents the intent. */
static inline uint16_t le16_to_cpu(uint16_t v) { return v; }
static inline uint16_t cpu_to_le16(uint16_t v) { return v; }
static inline uint32_t le32_to_cpu(uint32_t v) { return v; }
static inline uint32_t cpu_to_le32(uint32_t v) { return v; }
static inline uint64_t le64_to_cpu(uint64_t v) { return v; }
static inline uint64_t cpu_to_le64(uint64_t v) { return v; }

#define ALIGN_UP(x, a)  (((uint64_t)(x) + ((uint64_t)(a) - 1)) & ~((uint64_t)((a) - 1)))

static inline void vio_compiler_barrier(void)
{
    __asm__ __volatile__("" ::: "memory");
}

/* x86_64 is strongly ordered (TSO): store->store and load->load are
   architectural; only store->load needs a full fence. */
static inline void vio_wmb(void)  { vio_compiler_barrier(); }
static inline void vio_rmb(void)  { vio_compiler_barrier(); }
static inline void vio_mb(void)   { hal_cpu_memory_barrier(); }  /* mfence */

static inline int vring_need_event(uint16_t event_idx, uint16_t new_idx,
                                   uint16_t old_idx)
{
    return (uint16_t)(new_idx - event_idx - 1u) <
           (uint16_t)(new_idx - old_idx);
}

/* ---------------------------------------------------------------- *
 * Coherent DMA                                                       *
 * ---------------------------------------------------------------- */

/* Allocate zeroed, physically contiguous, coherent DMA memory.  DMA32 keeps
   every address below 4 GiB so the legacy 32-bit QUEUE_PFN and 64-bit modern
   queue addresses both work. */
static void *vnet_dma_alloc(size_t size, size_t align, uint64_t *dma_out)
{
    if (size == 0) { *dma_out = 0; return NULL; }
    (void)align;
    uint64_t npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    void *phys = pmm_alloc_frames_dma32(npages);
    if (!phys) return NULL;
    void *virt = vmm_mmap_phys((uint64_t)(uintptr_t)phys, (size_t)npages,
                               MMU_WRITE | MMU_UNCACHED);
    if (!virt) { pmm_free_frames(phys, npages); return NULL; }
    memset(virt, 0, size);
    *dma_out = (uint64_t)(uintptr_t)phys;
    return virt;
}

static void vnet_dma_free(void *virt, size_t size, uint64_t dma)
{
    if (!virt) return;
    uint64_t npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = 0; i < npages; i++)
        vmm_unmap_page((uint64_t)(uintptr_t)virt + i * PAGE_SIZE);
    pmm_free_frames((void *)(uintptr_t)dma, npages);
}

/* ---------------------------------------------------------------- *
 * Transport: PCI config + BAR access                                 *
 * ---------------------------------------------------------------- */

static uint8_t  t_cfg_read8 (const struct pci_device *dev, uint16_t off)
{ return pci_read8 (dev->bus, dev->dev, dev->func, off); }
static uint16_t t_cfg_read16(const struct pci_device *dev, uint16_t off)
{ return pci_read16(dev->bus, dev->dev, dev->func, off); }
static uint32_t t_cfg_read32(const struct pci_device *dev, uint16_t off)
{ return pci_read32(dev->bus, dev->dev, dev->func, off); }
static void     t_cfg_write16(const struct pci_device *dev, uint16_t off, uint16_t v)
{ pci_write16(dev->bus, dev->dev, dev->func, off, v); }
static void     t_cfg_write32(const struct pci_device *dev, uint16_t off, uint32_t v)
{ pci_write32(dev->bus, dev->dev, dev->func, off, v); }

struct vnet_transport {
    struct pci_device *pdev;
    int modern;
    int msix_enabled;
    uint8_t msix_cap_pos;

    /* Modern (capability-based) interface. */
    volatile uint8_t *common_base;   /* struct virtio_pci_common_cfg */
    volatile uint8_t *notify_base;
    uint32_t  notify_mult;
    size_t    notify_len;
    volatile uint8_t *isr;
    volatile uint8_t *devcfg;

    /* Legacy (BAR0 I/O) interface. */
    uint16_t io_port;                /* BAR0 I/O base */
    uint16_t cfg_window;             /* 0x14 or 0x18 (depends on MSI-X enable) */

    uint64_t offered_features;
    uint64_t negotiated_features;
};

static uint8_t t_get_status(struct vnet_transport *t)
{
    if (t->modern)
        return ((volatile struct virtio_pci_common_cfg *)t->common_base)
            ->device_status;
    return inb(t->io_port + 0x12);
}

static void t_set_status(struct vnet_transport *t, uint8_t s)
{
    if (t->modern)
        ((volatile struct virtio_pci_common_cfg *)t->common_base)
            ->device_status = s;
    else
        outb(t->io_port + 0x12, s);
}

static int t_reset(struct vnet_transport *t)
{
    t_set_status(t, 0);
    for (int i = 0; i < 1000; i++)
    {
        uint8_t s = t_get_status(t);
        if (s == 0) return 0;
        vio_mb();
        for (volatile int k = 0; k < 2000; k++) {}  /* brief pause */
    }
    return -1;
}

static uint8_t t_read_isr(struct vnet_transport *t)
{
    if (t->modern) return *t->isr;
    return inb(t->io_port + 0x13);
}

static uint64_t t_read_features(struct vnet_transport *t)
{
    if (!t->modern)
        return (uint64_t)inl(t->io_port + 0x00);  /* bits 0..31 only */
    volatile uint8_t *cfg = t->common_base;         /* common config region */
    /* device_feature_select @0x00, device_feature @0x04 */
    *(volatile uint32_t *)(cfg + 0x00) = 0;
    uint32_t lo = *(volatile uint32_t *)(cfg + 0x04);
    *(volatile uint32_t *)(cfg + 0x00) = 1;
    uint32_t hi = *(volatile uint32_t *)(cfg + 0x04);
    return ((uint64_t)hi << 32) | lo;
}

static void t_write_features(struct vnet_transport *t, uint64_t f)
{
    volatile struct virtio_pci_common_cfg *c =
        (volatile struct virtio_pci_common_cfg *)t->common_base;
    if (!t->modern) { outl(t->io_port + 0x04, (uint32_t)f); return; }
    c->driver_feature_select = 0;
    c->driver_feature = (uint32_t)f;
    c->driver_feature_select = 1;
    c->driver_feature = (uint32_t)(f >> 32);
}

/* Atomic generation-guarded device config read (modern).  Legacy reads
   byte-wise at the config window (no generation counter). */
static void t_cfg_read_dev(struct vnet_transport *t, uint16_t off,
                           void *dst, uint16_t len)
{
    uint8_t *p = (uint8_t *)dst;
    if (t->modern)
    {
        volatile struct virtio_pci_common_cfg *c =
            (volatile struct virtio_pci_common_cfg *)t->common_base;
        uint8_t g;
        do
        {
            g = c->config_generation;
            for (uint16_t i = 0; i < len; i++)
                p[i] = t->devcfg[off + i];
        } while (g != c->config_generation);
    }
    else
    {
        for (uint16_t i = 0; i < len; i++)
            p[i] = inb((uint16_t)(t->cfg_window + off + i));
    }
}

static int t_set_config_vector(struct vnet_transport *t, uint16_t vec)
{
    volatile struct virtio_pci_common_cfg *c =
        (volatile struct virtio_pci_common_cfg *)t->common_base;
    if (t->modern)
    {
        c->config_msix_vector = vec;
        return c->config_msix_vector == vec ? 0 : -1;
    }
    outw(t->io_port + 0x14, vec);
    return inw(t->io_port + 0x14) == vec ? 0 : -1;
}

static int t_set_queue_vector(struct vnet_transport *t, uint16_t q,
                              uint16_t vec)
{
    volatile struct virtio_pci_common_cfg *c =
        (volatile struct virtio_pci_common_cfg *)t->common_base;
    if (t->modern)
    {
        c->queue_select = q;
        c->queue_msix_vector = vec;
        return c->queue_msix_vector == vec ? 0 : -1;
    }
    outw(t->io_port + 0x0E, q);
    outw(t->io_port + 0x16, vec);
    return inw(t->io_port + 0x16) == vec ? 0 : -1;
}

/* PCI capability walk: record virtio capabilities (type 1-4) and MSI-X pos. */
static int t_discover(struct vnet_transport *t)
{
    struct pci_device *p = t->pdev;
    /* Use pci_read32 (correct) rather than pci_read16, which is buggy for
       odd offsets in this tree. */
    uint32_t id = pci_read32(p->bus, p->dev, p->func, PCI_VENDOR_ID);
    uint16_t vendor = (uint16_t)(id & 0xFFFFu);
    uint16_t did = (uint16_t)(id >> 16);
    if (vendor != VIRTIO_VENDOR_ID) return -1;
    int modern_dev = (did == VIRTIO_DEV_ID_NET_MODERN);
    /* Transitional net device (0x1000).  The spec wants subsystem_device == 1,
       but QEMU does not always program it; vendor 0x1AF4 + device 0x1000 is
       unambiguous for a transitional net device, so accept it here. */
    int trans_dev  = (did == VIRTIO_DEV_ID_NET_TRANS);
    if (!modern_dev && !trans_dev) return -1;

    /* One pass over the capability list. */
    int found[4] = {0, 0, 0, 0};   /* cfg_type 1..4 */
    uint8_t fbar[4] = {0};
    uint32_t foff[4] = {0};
    uint32_t flen[4] = {0};
    uint32_t fmult = 0;
    uint16_t msix_pos = 0;

    uint8_t pos = 0;
    uint32_t status = pci_read32(p->bus, p->dev, p->func, PCI_STATUS);
    if ((status >> 16) & PCI_STATUS_CAP_LIST)
        pos = (uint8_t)(t_cfg_read8(p, PCI_CAPABILITY_LIST) & 0xFC);

    int guard = 48;
    while (pos >= 0x40 && guard-- > 0)
    {
        uint8_t id  = t_cfg_read8(p, pos);
        uint8_t next = (uint8_t)(t_cfg_read8(p, pos + 1) & 0xFC);
        if (id == PCI_CAP_ID_VENDOR)
        {
            uint8_t len  = t_cfg_read8(p, pos + 2);
            uint8_t type = t_cfg_read8(p, pos + 3);
            uint8_t bar  = t_cfg_read8(p, pos + 4);
            if (len >= 16 && bar <= 5 && type >= 1 && type <= 4 &&
                !found[type - 1])
            {
                uint32_t off = t_cfg_read32(p, pos + 8);
                uint32_t length = t_cfg_read32(p, pos + 12);
                /* Reject malformed capabilities (not the device). */
                uint64_t bar_addr = pci_bar_addr(p, bar);
                if ((bar_addr & 1ULL) != 0) { pos = next; continue; }  /* must be MMIO */
                int ok = 1;
                if (type == VIRTIO_PCI_CAP_COMMON_CFG && length < 56) ok = 0;
                if (type == VIRTIO_PCI_CAP_NOTIFY_CFG && len < 20) ok = 0;
                if (type == VIRTIO_PCI_CAP_ISR_CFG     && length < 1) ok = 0;
                if (off + length < off) ok = 0;  /* overflow */
                if (ok)
                {
                    found[type - 1] = 1;
                    fbar[type - 1] = bar;
                    foff[type - 1] = off;
                    flen[type - 1] = length;
                    if (type == VIRTIO_PCI_CAP_NOTIFY_CFG)
                        fmult = t_cfg_read32(p, pos + 16);
                }
            }
        }
        else if (id == PCI_CAP_ID_MSIX)
        {
            msix_pos = pos;
        }
        pos = next;
    }

    t->modern = 0;
    t->msix_enabled = 0;
    t->msix_cap_pos = msix_pos;

    if (found[0] && found[1] && found[2] && found[3])
    {
        t->modern = 1;
        /* Map each BAR once; validate offset+length within BAR size. */
        volatile uint8_t *bv[6] = {NULL};
        size_t bl[6] = {0};
        for (int ty = 1; ty <= 4; ty++)
        {
            int b = (int)fbar[ty - 1];
            if (!bv[b])
            {
                uint64_t ba = pci_bar_addr(p, b);
                uint64_t sz = pci_bar_size(p, b);
                if (sz == 0) sz = 0x1000;
                uint64_t npages = (sz + PAGE_SIZE - 1) / PAGE_SIZE;
                bv[b] = (volatile uint8_t *)vmm_mmap_phys(ba & ~0xFULL,
                                                          (size_t)npages,
                                                          MMU_WRITE | MMU_UNCACHED);
                bl[b] = (size_t)sz;
            }
            if (!bv[b]) return -1;
            uint64_t off = foff[ty - 1];
            if (off + flen[ty - 1] > (uint64_t)bl[b] ||
                off + flen[ty - 1] < off) return -1;
            switch (ty)
            {
                case VIRTIO_PCI_CAP_COMMON_CFG:
                    t->common_base = bv[b] + off; break;
                case VIRTIO_PCI_CAP_NOTIFY_CFG:
                    t->notify_base = bv[b] + off;
                    t->notify_mult = fmult;
                    t->notify_len  = flen[ty - 1]; break;
                case VIRTIO_PCI_CAP_ISR_CFG:
                    t->isr = bv[b] + off; break;
                case VIRTIO_PCI_CAP_DEVICE_CFG:
                    t->devcfg = bv[b] + off; break;
            }
        }

        /* Modern interface is by definition 1.0+: VERSION_1 required. */
        uint64_t offered = t_read_features(t);
        t->offered_features = offered;
        /* NOTE: a modern-interface device is defined as 1.0+, so it should
           offer VIRTIO_F_VERSION_1.  Some transitional implementations omit
           it; we still proceed with the modern register interface. */
        (void)offered;
    }
    else
    {
        if (!trans_dev) return -1;
        /* Legacy fallback: BAR0 must be an I/O BAR. */
        uint64_t bar0 = pci_bar_addr(p, 0);
        if ((bar0 & 1ULL) == 0) return -1;
        t->modern = 0;
        t->io_port = (uint16_t)(bar0 & 0xFFFCu);
        /* Legacy config window is 0x14; moves to 0x18 once MSI-X enabled. */
        t->cfg_window = 0x14;
        t->offered_features = (uint64_t)inl(t->io_port + 0x00);
    }
    return 0;
}

/* ---------------------------------------------------------------- *
 * Split virtqueue engine                                             *
 * ---------------------------------------------------------------- */

struct vq {
    uint16_t index;       /* virtio queue index */
    uint16_t num;         /* ring size N */
    uint16_t mask;        /* N - 1 */
    int modern;           /* 1: ring fields little-endian */
    int event_idx;        /* VIRTIO_F_RING_EVENT_IDX negotiated */
    int indirect;         /* VIRTIO_F_RING_INDIRECT_DESC negotiated */
    int cb_enabled;       /* interrupts currently wanted */
    int broken;           /* device misbehaved */
    uint16_t avail_flags; /* shadow of avail->flags */
    uint16_t free_head;   /* head of free list (threaded via desc[].next) */
    uint16_t num_free;    /* free descriptors */
    uint16_t avail_shadow;/* driver copy of avail->idx */
    uint16_t last_used;   /* next used-ring entry to consume */
    uint16_t kick_idx;    /* avail idx at last doorbell decision */

    struct vring_desc  *desc;
    struct vring_avail *avail;
    struct vring_used  *used;
    uint16_t *used_event;   /* &avail->ring[num] */
    uint16_t *avail_event;  /* (uint16_t *)&used->ring[num] */
    struct vring_desc *ind_base; /* N * IND_MAX entries */
    uint64_t ind_dma;

    void **cookie;      /* per-head caller token */
    void  *base;  uint64_t base_dma;  size_t base_size;

    uintptr_t notify;   /* I/O port (legacy) or MMIO address (modern) */
    void (*notify_fn)(struct vq *q);
    spinlock_t lock;
};

static int vq_alloc(struct vq *q, uint16_t index, uint16_t num, int modern,
                    uint64_t feats)
{
    if (num == 0 || (num & (uint16_t)(num - 1)) || num > 32768)
        return -1;
    size_t used_off = (size_t)ALIGN_UP(16u * num + 6u + 2u * num, 4096);
    size_t total    = (size_t)ALIGN_UP(used_off + 6u + 8u * num, 4096);

    q->base = vnet_dma_alloc(total, 4096, &q->base_dma);
    if (!q->base) return -1;
    q->base_size = total;

    q->index = index;
    q->num = num;
    q->mask = (uint16_t)(num - 1);
    q->modern = modern;
    q->event_idx = !!(feats & VIRTIO_F_RING_EVENT_IDX);
    q->indirect = !!(feats & VIRTIO_F_RING_INDIRECT_DESC);
    q->cb_enabled = 1;
    q->broken = 0;
    q->avail_flags = 0;
    q->free_head = 0;
    q->num_free = num;
    q->avail_shadow = 0;
    q->last_used = 0;
    q->kick_idx = 0;

    q->desc = (struct vring_desc *)q->base;
    q->avail = (struct vring_avail *)((uint8_t *)q->base + 16u * num);
    q->used = (struct vring_used  *)((uint8_t *)q->base + used_off);
    q->used_event = &q->avail->ring[num];
    q->avail_event = (uint16_t *)&q->used->ring[num];

    q->cookie = (void **)kmalloc(sizeof(void *) * (size_t)num);
    if (!q->cookie) { vnet_dma_free(q->base, total, q->base_dma); return -1; }
    memset(q->cookie, 0, sizeof(void *) * (size_t)num);

    if (q->indirect)
    {
        size_t isz = (size_t)num * (size_t)IND_MAX * sizeof(struct vring_desc);
        q->ind_base = (struct vring_desc *)vnet_dma_alloc(isz, 16, &q->ind_dma);
        if (!q->ind_base) q->indirect = 0;  /* degrade gracefully */
    }
    else
    {
        q->ind_base = NULL;
        q->ind_dma = 0;
    }

    /* Thread the free list through desc[].next. */
    for (uint16_t i = 0; i < num; i++)
        *(volatile uint16_t *)&q->desc[i].next = (uint16_t)(i + 1);
    /* spinlock zero-initialised by the struct memset (locked == 0). */
    return 0;
}

static void vq_free_mem(struct vq *q)
{
    if (q->ind_base)
        vnet_dma_free(q->ind_base, (size_t)q->num * (size_t)IND_MAX *
                           sizeof(struct vring_desc), q->ind_dma);
    kfree(q->cookie);
    if (q->base)
        vnet_dma_free(q->base, q->base_size, q->base_dma);
    q->base = NULL; q->cookie = NULL; q->ind_base = NULL;
}

static int vq_alloc_chain(struct vq *q, uint16_t total, uint16_t *head_out)
{
    if (q->broken || total == 0 || total > q->num || q->num_free < total)
        return -1;
    uint16_t head = q->free_head;
    uint16_t i = head;
    uint16_t prev = 0xFFFFu;
    for (uint16_t k = 0; k < total; k++)
    {
        uint16_t nxt = *(volatile uint16_t *)&q->desc[i].next;
        if (prev != 0xFFFFu)
            *(volatile uint16_t *)&q->desc[prev].next = i;
        prev = i;
        i = nxt;
        if (i >= q->num) { q->broken = 1; return -1; }
    }
    q->free_head = i;
    q->num_free = (uint16_t)(q->num_free - total);
    *head_out = head;
    return 0;
}

static void vq_detach(struct vq *q, uint16_t head)
{
    uint16_t i = head, n = 1;
    while ((*(volatile uint16_t *)&q->desc[i].flags) & VRING_DESC_F_NEXT)
    {
        i = *(volatile uint16_t *)&q->desc[i].next;
        if (i >= q->num || ++n > q->num) { q->broken = 1; return; }
    }
    *(volatile uint16_t *)&q->desc[i].next = q->free_head;
    q->free_head = head;
    q->num_free = (uint16_t)(q->num_free + n);
}

/* Fill descriptor fields and publish one chain on the avail ring.
   sg[0..n_out) device-readable, sg[n_out..n_out+n_in) device-writable.
   `cookie` is returned by vq_get_buf on completion.  Caller holds q->lock. */
static int vq_add(struct vq *q, const struct vnet_sg *sg, uint16_t n_out,
                  uint16_t n_in, void *cookie)
{
    uint16_t total = (uint16_t)(n_out + n_in);
    if (q->broken) return -1;
    if (!cookie || total == 0 || total > q->num) return -1;

    uint16_t head;
    if (vq_alloc_chain(q, total, &head) < 0) return -1;

    if (q->indirect && total > 1 && total <= IND_MAX)
    {
        struct vring_desc *tbl = q->ind_base + (size_t)head * IND_MAX;
        uint64_t tbl_dma = q->ind_dma +
            (uint64_t)head * (uint64_t)IND_MAX * sizeof(struct vring_desc);
        for (uint16_t k = 0; k < total; k++)
        {
            uint16_t fl = (uint16_t)(((k >= n_out) ? VRING_DESC_F_WRITE : 0) |
                                     ((k + 1u < total) ? VRING_DESC_F_NEXT : 0));
            *(volatile uint64_t *)&tbl[k].addr = sg[k].dma;
            *(volatile uint32_t *)&tbl[k].len  = sg[k].len;
            *(volatile uint16_t *)&tbl[k].flags = fl;
            *(volatile uint16_t *)&tbl[k].next  = (uint16_t)(k + 1);
        }
        *(volatile uint64_t *)&q->desc[head].addr = tbl_dma;
        *(volatile uint32_t *)&q->desc[head].len  = (uint32_t)total *
            (uint32_t)sizeof(struct vring_desc);
        *(volatile uint16_t *)&q->desc[head].flags = VRING_DESC_F_INDIRECT;
    }
    else
    {
        uint16_t i = head;
        for (uint16_t k = 0; k < total; k++)
        {
            uint16_t fl = (uint16_t)(((k >= n_out) ? VRING_DESC_F_WRITE : 0) |
                                     ((k + 1u < total) ? VRING_DESC_F_NEXT : 0));
            *(volatile uint64_t *)&q->desc[i].addr = sg[k].dma;
            *(volatile uint32_t *)&q->desc[i].len  = sg[k].len;
            *(volatile uint16_t *)&q->desc[i].flags = fl;
            i = *(volatile uint16_t *)&q->desc[i].next;
        }
    }

    q->cookie[head] = cookie;
    *(volatile uint16_t *)&q->avail->ring[q->avail_shadow & q->mask] = head;
    vio_wmb();                                        /* desc + slot before idx */
    q->avail_shadow++;
    *(volatile uint16_t *)&q->avail->idx = q->avail_shadow;
    return (int)head;
}

static int vq_kick_prepare(struct vq *q)
{
    uint16_t new_idx = q->avail_shadow;
    uint16_t old_idx = q->kick_idx;
    if (new_idx == old_idx) return 0;
    vio_mb();                                         /* idx store -> flags load */
    int need;
    if (q->event_idx)
        need = vring_need_event(*(volatile uint16_t *)q->avail_event,
                                new_idx, old_idx);
    else
        need = !((*(volatile uint16_t *)&q->used->flags) &
                 VRING_USED_F_NO_NOTIFY);
    q->kick_idx = new_idx;
    return need;
}

static inline void vq_notify(struct vq *q)
{
    q->notify_fn(q);
}

static void *vq_get_buf(struct vq *q, uint32_t *len)
{
    uint16_t used_idx = *(volatile uint16_t *)&q->used->idx;
    if (used_idx == q->last_used) return NULL;
    if ((uint16_t)(used_idx - q->last_used) > q->num) { q->broken = 1; return NULL; }
    vio_rmb();
    struct vring_used_elem *e = &q->used->ring[q->last_used & q->mask];
    uint32_t id = *(volatile uint32_t *)&e->id;
    *len = *(volatile uint32_t *)&e->len;
    if (id >= q->num || q->cookie[id] == NULL) { q->broken = 1; return NULL; }
    void *cookie = q->cookie[id];
    q->cookie[id] = NULL;
    vq_detach(q, (uint16_t)id);
    q->last_used++;
    if (q->event_idx && q->cb_enabled)
    {
        *(volatile uint16_t *)q->used_event = q->last_used;
        vio_mb();
    }
    return cookie;
}

static void vq_disable_cb(struct vq *q)
{
    if (!q->cb_enabled) return;
    q->cb_enabled = 0;
    if (!q->event_idx)
    {
        q->avail_flags |= VRING_AVAIL_F_NO_INTERRUPT;
        *(volatile uint16_t *)&q->avail->flags = q->avail_flags;
    }
}

static uint16_t vq_enable_cb_prepare(struct vq *q)
{
    q->cb_enabled = 1;
    if (q->event_idx)
        *(volatile uint16_t *)q->used_event = q->last_used;
    else
    {
        q->avail_flags &= (uint16_t)~VRING_AVAIL_F_NO_INTERRUPT;
        *(volatile uint16_t *)&q->avail->flags = q->avail_flags;
    }
    vio_mb();
    return q->last_used;
}

static uint16_t vq_enable_cb_delayed(struct vq *q)
{
    q->cb_enabled = 1;
    if (q->event_idx)
    {
        uint16_t bufs = (uint16_t)(((uint16_t)(q->avail_shadow - q->last_used) *
                                     3u) / 4u);
        *(volatile uint16_t *)q->used_event = (uint16_t)(q->last_used + bufs);
    }
    else
    {
        q->avail_flags &= (uint16_t)~VRING_AVAIL_F_NO_INTERRUPT;
        *(volatile uint16_t *)&q->avail->flags = q->avail_flags;
    }
    vio_mb();
    return q->last_used;
}

static inline int vq_has_pending(struct vq *q, uint16_t snapshot)
{
    return *(volatile uint16_t *)&q->used->idx != snapshot;
}

/* ---------------------------------------------------------------- *
 * virtio-net device state                                            *
 * ---------------------------------------------------------------- */

struct net_tx {
    struct vnet_sg frag[VNET_MAX_FRAGS];
    uint16_t  nfrag;
    uint32_t  len;
    uint8_t   csum_needed;
    uint8_t   gso_type;
    uint16_t  hdr_len, gso_size, csum_start, csum_offset;
    struct mbuf *mbuf;
};

struct tx_req {
    struct net_tx tx;
    uint16_t slot;
};

struct vnet {
    struct vnet *next;
    struct vnet_transport tp;
    struct netdev netdev;

    uint64_t features;
    int modern;
    uint16_t hdr_len;
    int mrg;
    int any_layout;
    uint8_t mac[6];
    uint16_t mtu;
    int link_up;

    struct vq rxq, txq, cvq;

    uint8_t *rx_pool;  uint64_t rx_pool_dma;
    uint32_t rx_buf_len;  uint16_t rx_nbuf;

    uint8_t *tx_hdr;  uint64_t tx_hdr_dma;
    struct tx_req *tx_req;
    uint16_t *tx_free;  uint16_t tx_nfree;
    int tx_stopped;

    uint8_t *ctrl_buf; uint64_t ctrl_dma;
    spinlock_t ctrl_lock;

    volatile uint32_t cfg_changed;
    volatile uint32_t reset_pending;
};

static struct vnet *g_vnet_list = NULL;
static void vnet_work(struct vnet *n);

/* ---------------------------------------------------------------- *
 * Feature selection (spec section 1.4)                               *
 * ---------------------------------------------------------------- */

static uint64_t vnet_select_features(uint64_t offered, int modern)
{
    uint64_t want = VIRTIO_F_RING_INDIRECT_DESC | VIRTIO_F_RING_EVENT_IDX |
                    VNET_F_MAC | VNET_F_STATUS | VNET_F_MTU | VNET_F_CSUM |
                    VNET_F_GUEST_CSUM | VNET_F_HOST_TSO4 | VNET_F_HOST_TSO6 |
                    VNET_F_HOST_ECN | VNET_F_MRG_RXBUF | VNET_F_CTRL_VQ |
                    VNET_F_CTRL_RX;
    if (modern) want |= VIRTIO_F_VERSION_1 | VIRTIO_F_ACCESS_PLATFORM;

    uint64_t neg = offered & want;   /* never acknowledge unknown bits */
    if (!(neg & VNET_F_CSUM))
        neg &= ~(VNET_F_HOST_TSO4 | VNET_F_HOST_TSO6 | VNET_F_HOST_ECN);
    if (!(neg & VNET_F_CTRL_VQ))
        neg &= ~VNET_F_CTRL_RX;
    return neg;
}

/* ---------------------------------------------------------------- *
 * virtqueue notify functions                                         *
 * ---------------------------------------------------------------- */

static void vq_notify_modern(struct vq *q)
{
    *(volatile uint16_t *)q->notify = q->index;
}

static void vq_notify_legacy(struct vq *q)
{
    outw((uint16_t)q->notify, q->index);
}

/* ---------------------------------------------------------------- *
 * Queue setup                                                        *
 * ---------------------------------------------------------------- */

/* Allocate rings and program the queue on the transport.
   `max_size` is the largest size this driver is willing to use.
   Returns 0 on success. */
static int vnet_queue_setup(struct vnet *n, struct vq *q, uint16_t idx,
                            uint16_t max_size)
{
    struct vnet_transport *t = &n->tp;
    uint16_t sz;

    if (t->modern)
    {
        volatile struct virtio_pci_common_cfg *c =
            (volatile struct virtio_pci_common_cfg *)t->common_base;
        c->queue_select = idx;
        sz = c->queue_size;
        if (sz == 0) return -1;                    /* queue does not exist */
        if (c->queue_enable) return -1;            /* must be disabled after reset */
        if (sz > max_size)
        {
            c->queue_size = max_size;
            sz = c->queue_size;
        }
    }
    else
    {
        outw(t->io_port + 0x0E, idx);
        sz = inw(t->io_port + 0x0C);
        if (sz == 0) return -1;                    /* queue does not exist */
        if (inl(t->io_port + 0x08) != 0) return -1; /* already enabled */
        if (sz > max_size) return -1;              /* legacy cannot shrink */
    }

    int rc = vq_alloc(q, idx, sz, t->modern, n->features);
    if (rc < 0) return rc;

    if (t->modern)
    {
        volatile struct virtio_pci_common_cfg *c =
            (volatile struct virtio_pci_common_cfg *)t->common_base;
        uint64_t desc  = q->base_dma;
        uint64_t avail = q->base_dma + 16u * (uint64_t)sz;
        uint64_t used  = q->base_dma +
            (uint64_t)((uint8_t *)q->used - (uint8_t *)q->base);

        /* 64-bit fields as two 32-bit stores, low half first.  The common
           config layout: queue_desc @0x20, queue_driver @0x28,
           queue_device @0x30. */
        *(volatile uint32_t *)((volatile uint8_t *)c + 0x20) =
            (uint32_t)(desc & 0xFFFFFFFFu);
        *(volatile uint32_t *)((volatile uint8_t *)c + 0x24) =
            (uint32_t)(desc >> 32);
        *(volatile uint32_t *)((volatile uint8_t *)c + 0x28) =
            (uint32_t)(avail & 0xFFFFFFFFu);
        *(volatile uint32_t *)((volatile uint8_t *)c + 0x2C) =
            (uint32_t)(avail >> 32);
        *(volatile uint32_t *)((volatile uint8_t *)c + 0x30) =
            (uint32_t)(used & 0xFFFFFFFFu);
        *(volatile uint32_t *)((volatile uint8_t *)c + 0x34) =
            (uint32_t)(used >> 32);

        uint16_t noff = c->queue_notify_off;
        if ((size_t)noff * t->notify_mult + 2 > t->notify_len)
        { vq_free_mem(q); return -1; }
        q->notify = (uintptr_t)(t->notify_base +
                                (size_t)noff * t->notify_mult);
        q->notify_fn = vq_notify_modern;
    }
    else
    {
        outl(t->io_port + 0x08, (uint32_t)(q->base_dma >> 12));
        q->notify = (uintptr_t)(t->io_port + 0x10);
        q->notify_fn = vq_notify_legacy;
    }
    return 0;
}

static void vnet_queue_enable(struct vnet *n, uint16_t idx)
{
    if (!n->tp.modern) return;
    volatile struct virtio_pci_common_cfg *c =
        (volatile struct virtio_pci_common_cfg *)n->tp.common_base;
    c->queue_select = idx;
    c->queue_enable = 1;
}

static void vnet_queue_disable(struct vnet *n, uint16_t idx)
{
    if (n->tp.modern) return;
    outw(n->tp.io_port + 0x0E, idx);
    outl(n->tp.io_port + 0x08, 0);
}

/* ---------------------------------------------------------------- *
 * Rx buffer pool                                                     *
 * ---------------------------------------------------------------- */

/* Post one Rx buffer onto the receive queue.  Caller holds rxq.lock. */
static int vnet_rx_post(struct vnet *n, uint8_t *buf)
{
    struct vq *q = &n->rxq;
    struct vnet_sg sg[2];
    uint64_t dma = n->rx_pool_dma + (uint64_t)(buf - n->rx_pool);

    if (n->mrg || n->any_layout)
    {
        sg[0].dma = dma; sg[0].len = n->rx_buf_len;
        return vq_add(q, sg, 0, 1, buf);
    }
    /* Legacy, no ANY_LAYOUT: header in its own descriptor. */
    sg[0].dma = dma;               sg[0].len = n->hdr_len;
    sg[1].dma = dma + n->hdr_len;  sg[1].len = n->rx_buf_len - n->hdr_len;
    return vq_add(q, sg, 0, 2, buf);
}

static void vnet_rx_fill(struct vnet *n)
{
    int kick = 0;
    unsigned long flags = spin_lock_irq(&n->rxq.lock);
    for (uint16_t i = 0; i < n->rx_nbuf; i++)
    {
        if (vnet_rx_post(n, n->rx_pool + (size_t)i * n->rx_buf_len) < 0)
            break;
    }
    kick = vq_kick_prepare(&n->rxq);
    spin_unlock_irq(&n->rxq.lock, flags);
    if (kick) vq_notify(&n->rxq);
}

/* ---------------------------------------------------------------- *
 * Transmit path                                                      *
 * ---------------------------------------------------------------- */

/* Reclaim completed Tx descriptors.  Caller holds txq.lock. */
static int vnet_tx_reclaim_locked(struct vnet *n)
{
    struct vq *q = &n->txq;
    uint32_t len;
    int cnt = 0;
    while (1)
    {
        struct tx_req *r = (struct tx_req *)vq_get_buf(q, &len);
        if (!r) break;
        if (n->tx_free) n->tx_free[n->tx_nfree++] = r->slot;
        if (r->tx.mbuf) mbuf_free(r->tx.mbuf);
        cnt++;
    }
    if (n->tx_stopped && q->num_free >= VNET_MAX_FRAGS + 1 &&
        n->tx_nfree > 0)
    {
        n->tx_stopped = 0;
        netdev_wake_queue(&n->netdev);
    }
    return cnt;
}

static int vnet_build_tx_sg(struct vnet *n, struct mbuf *m,
                            struct net_tx *ntx)
{
    (void)n;
    uint16_t k = 0;
    ntx->nfrag = 0;
    ntx->len = 0;

    /* Walk the mbuf chain: first segment is the Ethernet header (from
       ethernet_send), then the payload chain via next_seg. */
    struct mbuf *seg = m;
    while (seg && ntx->nfrag < VNET_MAX_FRAGS)
    {
        ntx->frag[k].dma = (uint64_t)seg->phys + seg->data_off;
        ntx->frag[k].len = seg->len;
        ntx->len += seg->len;
        k++;
        ntx->nfrag++;
        seg = seg->next_seg;
    }
    if (seg) return -1;  /* too many fragments for our fixed sg array */
    return 0;
}

static int vnet_netdev_tx(struct netdev *dev, struct mbuf *m)
{
    struct vnet *n = (struct vnet *)dev->priv;
    struct vq *q = &n->txq;
    struct vnet_sg sg[VNET_MAX_FRAGS + 1];
    uint16_t slot;
    int rc, kick;

    if (!m) return -1;
    if (m->len == 0 && !m->next_seg) { mbuf_free(m); return -1; }

    unsigned long flags = spin_lock_irq(&q->lock);
    vnet_tx_reclaim_locked(n);
    if (n->tx_nfree == 0)
    {
        n->tx_stopped = 1;
        spin_unlock_irq(&q->lock, flags);
        netdev_stop_queue(&n->netdev);
        mbuf_free(m);
        return -1;
    }

    slot = n->tx_free[--n->tx_nfree];
    struct virtio_net_hdr_mrg_rxbuf *h =
        (struct virtio_net_hdr_mrg_rxbuf *)(n->tx_hdr + (size_t)slot * 16u);
    memset((void *)h, 0, 16);  /* whole 16-byte slot, num_buffers = 0 */

    /* Build the frame segment list from the mbuf chain. */
    struct net_tx ntx;
    memset(&ntx, 0, sizeof(ntx));
    ntx.mbuf = m;
    if (vnet_build_tx_sg(n, m, &ntx) < 0)
    {
        n->tx_free[n->tx_nfree++] = slot;
        spin_unlock_irq(&q->lock, flags);
        mbuf_free(m);
        return -1;
    }

    /* Fill the virtio-net header. */
    if (ntx.csum_needed)
    {
        h->hdr.flags = VNET_HDR_F_NEEDS_CSUM;
        h->hdr.csum_start = cpu_to_le16(ntx.csum_start);
        h->hdr.csum_offset = cpu_to_le16(ntx.csum_offset);
    }
    if (ntx.gso_type != VNET_HDR_GSO_NONE)
    {
        h->hdr.gso_type = ntx.gso_type;
        h->hdr.hdr_len = cpu_to_le16(ntx.hdr_len);
        h->hdr.gso_size = cpu_to_le16(ntx.gso_size);
    }

    n->tx_req[slot].tx = ntx;
    n->tx_req[slot].slot = slot;

    sg[0].dma = n->tx_hdr_dma + (uint64_t)slot * 16u;
    sg[0].len = n->hdr_len;
    for (uint16_t i = 0; i < ntx.nfrag; i++) sg[i + 1] = ntx.frag[i];

    rc = vq_add(q, sg, (uint16_t)(ntx.nfrag + 1), 0, &n->tx_req[slot]);
    if (rc < 0)
    {
        /* Ring full. */
        n->tx_free[n->tx_nfree++] = slot;
        n->tx_stopped = 1;
        uint16_t snap = vq_enable_cb_delayed(q);
        int more = vq_has_pending(q, snap);
        spin_unlock_irq(&q->lock, flags);
        netdev_stop_queue(&n->netdev);
        mbuf_free(m);
        if (more)
        {
            flags = spin_lock_irq(&n->txq.lock);
            vnet_tx_reclaim_locked(n);
            spin_unlock_irq(&n->txq.lock, flags);
        }
        return -1;
    }
    if (!q->indirect && q->num_free < VNET_MAX_FRAGS + 1)
    {
        n->tx_stopped = 1;
        netdev_stop_queue(&n->netdev);
    }
    kick = vq_kick_prepare(q);
    spin_unlock_irq(&q->lock, flags);
    if (kick) vq_notify(q);
    return 0;
}

/* ---------------------------------------------------------------- *
 * Receive path                                                       *
 * ---------------------------------------------------------------- */

static void vnet_rx_deliver(struct vnet *n, struct mbuf *m,
                            int csum_state, uint16_t csum_start,
                            uint16_t csum_offset)
{
    (void)csum_state; (void)csum_start; (void)csum_offset;
    netdev_rx_poll(&n->netdev, m);
}

int vnet_rx_poll(struct vnet *n, int budget)
{
    struct vq *q = &n->rxq;
    int done = 0;

    while (done < budget)
    {
        uint8_t *bufs[16];
        uint32_t lens[16];
        uint16_t nb = 1, got = 0;
        uint32_t total = 0;
        int bad = 0;

        unsigned long flags = spin_lock_irq(&q->lock);
        bufs[0] = (uint8_t *)vq_get_buf(q, &lens[0]);
        if (!bufs[0]) { spin_unlock_irq(&q->lock, flags); break; }
        got = 1;

        struct virtio_net_hdr_mrg_rxbuf *h =
            (struct virtio_net_hdr_mrg_rxbuf *)bufs[0];
        if (lens[0] < n->hdr_len) bad = 1;
        if (!bad && n->mrg)
        {
            nb = le16_to_cpu(h->num_buffers);
            if (nb == 0 || nb > 16) bad = 1;
        }
        for (; !bad && got < nb; got++)
        {
            bufs[got] = (uint8_t *)vq_get_buf(q, &lens[got]);
            if (!bufs[got]) { bad = 1; break; }
        }
        spin_unlock_irq(&q->lock, flags);

        if (!bad)
        {
            total = lens[0] - n->hdr_len;
            for (uint16_t i = 1; i < nb; i++) total += lens[i];
            if (total < 14 || total > (uint32_t)n->mtu + 18u + 4u)
                bad = 1;
            else if (h->hdr.gso_type != VNET_HDR_GSO_NONE)
                bad = 1;  /* no GUEST_TSO* negotiated */
        }

        struct mbuf *m = NULL;
        if (!bad)
        {
            m = mbuf_alloc0();
            if (m)
            {
                uint32_t off = 0, c;
                c = lens[0] - n->hdr_len;
                memcpy(m->data + m->data_off, bufs[0] + n->hdr_len, c);
                m->len = c; off = c;
                for (uint16_t i = 1; i < nb; i++)
                {
                    memcpy(m->data + m->data_off + off, bufs[i], lens[i]);
                    off += lens[i];
                }
                m->len = total;
            }
        }

        int csum_state = 0;  /* none */
        if (!bad && m && (n->features & VNET_F_GUEST_CSUM))
        {
            if (h->hdr.flags & VNET_HDR_F_DATA_VALID) csum_state = 1;   /* verified */
            else if (h->hdr.flags & VNET_HDR_F_NEEDS_CSUM) csum_state = 2; /* partial */
        }
        if (m)
        {
            /* Deliver with no lock held: the stack may call vnet_netdev_tx. */
            vnet_rx_deliver(n, m, csum_state,
                            le16_to_cpu(h->hdr.csum_start),
                            le16_to_cpu(h->hdr.csum_offset));
        }

        /* Recycle every buffer we took (even dropped ones). */
        flags = spin_lock_irq(&q->lock);
        for (uint16_t i = 0; i < got; i++)
        {
            if (bufs[i]) vnet_rx_post(n, bufs[i]);
        }
        int kick = vq_kick_prepare(q);
        spin_unlock_irq(&q->lock, flags);
        if (kick) vq_notify(q);

        if (q->broken)
        {
            n->reset_pending = 1;
            break;
        }
        done++;
    }
    return done;
}

/* ---------------------------------------------------------------- *
 * Control virtqueue                                                  *
 * ---------------------------------------------------------------- */

/* Command layout: { class, cmd, data... } readable, then one writable ack byte. */
static int vnet_ctrl_cmd(struct vnet *n, uint8_t cls, uint8_t cmd,
                         const void *data, uint16_t dlen)
{
    struct vq *q = &n->cvq;
    uint32_t len;
    int rc;

    if (dlen > 56) return -1;
    spin_lock(&n->ctrl_lock);

    uint8_t *b = n->ctrl_buf;
    b[0] = cls; b[1] = cmd;
    if (dlen) memcpy(b + 2, data, dlen);
    b[2 + dlen] = 0xFF;  /* ack: 0 = OK, 1 = ERR */

    struct vnet_sg sg[2];
    sg[0].dma = n->ctrl_dma;              sg[0].len = (uint32_t)(2 + dlen);
    sg[1].dma = n->ctrl_dma + 2 + dlen;   sg[1].len = 1;

    unsigned long flags = spin_lock_irq(&q->lock);
    rc = vq_add(q, sg, 1, 1, b);
    int kick = (rc >= 0) ? vq_kick_prepare(q) : 0;
    spin_unlock_irq(&q->lock, flags);
    if (rc < 0) { spin_unlock(&n->ctrl_lock); return -1; }
    if (kick) vq_notify(q);

    /* Control queue runs with callbacks off: poll for completion. */
    for (uint32_t t = 0; ; t++)
    {
        flags = spin_lock_irq(&q->lock);
        void *c = vq_get_buf(q, &len);
        spin_unlock_irq(&q->lock, flags);
        if (c) break;
        if (t > 200000u) { spin_unlock(&n->ctrl_lock); return -1; }
        for (volatile int k = 0; k < 1000; k++) {}  /* ~10 us */
    }
    rc = (b[2 + dlen] == 0) ? 0 : -1;
    spin_unlock(&n->ctrl_lock);
    return rc;
}

/* ---------------------------------------------------------------- *
 * Interrupts (MSI-X primary, INTx fallback)                          *
 * ---------------------------------------------------------------- */

#define MSIX_TABLE_ENTRY_BYTES 16

static void vnet_irq_config(void *ctx, void *frame);
static void vnet_irq_queues(void *ctx, void *frame);
static void vnet_irq_intx(void *ctx, void *frame);

static int vnet_msix_program(struct vnet *n)
{
    struct vnet_transport *t = &n->tp;
    if (!t->msix_cap_pos) return -1;

    struct pci_device *p = t->pdev;
    uint16_t mc = t_cfg_read16(p, (uint16_t)(t->msix_cap_pos + 2));
    int table_size = (int)(mc & 0x7FFu) + 1;
    uint32_t to = t_cfg_read32(p, (uint16_t)(t->msix_cap_pos + 4));
    int tbir = (int)(to & 0x7u);
    uint64_t tbase = (pci_bar_addr(p, tbir) & ~0xFULL) +
                     (uint64_t)(to & ~0x7FULL);
    volatile uint8_t *tb = (volatile uint8_t *)hal_mmio_map_phys(
        tbase, (size_t)table_size * MSIX_TABLE_ENTRY_BYTES);
    if (!tb) return -1;

    /* Vectors: 0=config, 1=rx, 2=tx at IDT base VNET_MSI_VECTOR_BASE. */
    uint8_t vec[3] = {
        (uint8_t)(VNET_MSI_VECTOR_BASE + 0),
        (uint8_t)(VNET_MSI_VECTOR_BASE + 1),
        (uint8_t)(VNET_MSI_VECTOR_BASE + 2),
    };

    for (int i = 0; i < 3; i++)
    {
        uint64_t addr = 0xFEE00000ULL;  /* x86 MSI: physical dest, CPU 0 */
        *(volatile uint32_t *)(tb + i * MSIX_TABLE_ENTRY_BYTES + 0) =
            (uint32_t)(addr & 0xFFFFFFFFu);
        *(volatile uint32_t *)(tb + i * MSIX_TABLE_ENTRY_BYTES + 4) =
            (uint32_t)(addr >> 32);
        *(volatile uint32_t *)(tb + i * MSIX_TABLE_ENTRY_BYTES + 8) = vec[i];
        *(volatile uint32_t *)(tb + i * MSIX_TABLE_ENTRY_BYTES + 12) = 0; /* unmask */
    }
    vio_mb();

    /* Enable MSI-X: table size field preserved, function mask clear, enable set. */
    t_cfg_write16(p, (uint16_t)(t->msix_cap_pos + 2),
                  (uint16_t)((mc & 0x7FFu) | (1u << 13)));

    /* Register handlers. */
    if (hal_irq_register(vec[0], vnet_irq_config, n) != 0) return -1;
    if (hal_irq_register(vec[1], vnet_irq_queues, n) != 0) return -1;
    if (vec[2] != vec[1])
        if (hal_irq_register(vec[2], vnet_irq_queues, n) != 0) return -1;

    t->msix_enabled = 1;
    return 0;
}

static void vnet_irq_config(void *ctx, void *frame)
{
    struct vnet *n = (struct vnet *)ctx;
    (void)frame;
    n->cfg_changed = 1;
}

static void vnet_irq_queues(void *ctx, void *frame)
{
    struct vnet *n = (struct vnet *)ctx;
    unsigned long flags;
    (void)frame;
    flags = spin_lock_irq(&n->rxq.lock); vq_disable_cb(&n->rxq); spin_unlock_irq(&n->rxq.lock, flags);
    flags = spin_lock_irq(&n->txq.lock); vq_disable_cb(&n->txq); spin_unlock_irq(&n->txq.lock, flags);
}

static void vnet_irq_intx(void *ctx, void *frame)
{
    struct vnet *n = (struct vnet *)ctx;
    unsigned long flags;
    uint8_t isr;
    (void)frame;
    isr = t_read_isr(&n->tp);
    if (isr == 0) return;  /* not our interrupt */
    if (isr & 2) { n->cfg_changed = 1; }
    if (isr & 1)
    {
        flags = spin_lock_irq(&n->rxq.lock); vq_disable_cb(&n->rxq); spin_unlock_irq(&n->rxq.lock, flags);
        flags = spin_lock_irq(&n->txq.lock); vq_disable_cb(&n->txq); spin_unlock_irq(&n->txq.lock, flags);
    }
}

/* ---------------------------------------------------------------- *
 * Bring-up / teardown                                                *
 * ---------------------------------------------------------------- */

static int vnet_bringup(struct vnet *n)
{
    struct vnet_transport *t = &n->tp;
    int rc;

    /* S0: reset. */
    if ((rc = t_reset(t)) < 0) return rc;

    /* S1, S2: ACKNOWLEDGE, DRIVER. */
    uint8_t st = VIRTIO_STATUS_ACK;
    t_set_status(t, st);
    st |= VIRTIO_STATUS_DRIVER;
    t_set_status(t, st);

    /* S3: read offered features, write driver features. */
    uint64_t offered = t_read_features(t);
    t->offered_features = offered;
    if (t->modern && !(offered & VIRTIO_F_VERSION_1)) return -1;
    uint64_t neg = vnet_select_features(offered, t->modern);
    if (!t->modern) neg &= 0xFFFFFFFFull;
    t_write_features(t, neg);
    n->features = neg;

    if (t->modern)
    {
        st |= VIRTIO_STATUS_FEATURES_OK;
        t_set_status(t, st);
        if (!(t_get_status(t) & VIRTIO_STATUS_FEATURES_OK)) return -1;
    }

    n->mrg = !!(neg & VNET_F_MRG_RXBUF);
    n->hdr_len = (neg & (VIRTIO_F_VERSION_1 | VNET_F_MRG_RXBUF)) ? 12 : 10;
    n->any_layout = t->modern || !!(offered & (1ull << 27));
    if (!t->modern) n->any_layout = 0;

    /* MSI-X setup first (legacy config offset depends on it). */
    if (vnet_msix_program(n) != 0) t->msix_enabled = 0;
    t->cfg_window = (uint16_t)(t->msix_enabled ? 0x18 : 0x14);

    /* Read device config. */
    t_cfg_read_dev(t, 0, n->mac, 6);
    if (!(n->features & VNET_F_MAC))
    {
        /* locally administered unicast MAC */
        n->mac[0] = 0x52; n->mac[1] = 0x54; n->mac[2] = 0x00;
        n->mac[3] = 0x12; n->mac[4] = 0x34; n->mac[5] = 0x56;
        n->mac[0] = (uint8_t)((n->mac[0] & 0xFEu) | 0x02u);
    }
    uint16_t mtu = 1500;
    if (n->features & VNET_F_MTU)
    {
        t_cfg_read_dev(t, 10, &mtu, 2);
        mtu = le16_to_cpu(mtu);
        if (mtu < 68) mtu = 1500;
    }
    n->mtu = mtu;

    uint16_t stcfg = 0;
    if (n->features & VNET_F_STATUS)
    {
        t_cfg_read_dev(t, 6, &stcfg, 2);
        stcfg = le16_to_cpu(stcfg);
        n->link_up = (stcfg & 1) ? 1 : 0;
    }
    else n->link_up = 1;

    /* S6: queues. */
    if ((rc = vnet_queue_setup(n, &n->rxq, 0, VNET_QUEUE_SIZE)) < 0) return rc;
    if ((rc = vnet_queue_setup(n, &n->txq, 1, VNET_QUEUE_SIZE)) < 0)
    { vnet_queue_disable(n, 0); vq_free_mem(&n->rxq); return rc; }
    if (n->features & VNET_F_CTRL_VQ)
    {
        if (vnet_queue_setup(n, &n->cvq, 2, VNET_CTRL_QUEUE_SIZE) < 0)
        {
            n->features &= ~(VNET_F_CTRL_VQ | VNET_F_CTRL_RX);
        }
    }
    vq_disable_cb(&n->cvq);  /* control queue is polled */

    /* Bind MSI-X vectors to queues. */
    if (t->msix_enabled)
    {
        if (t_set_config_vector(t,
                (uint16_t)(VNET_MSI_VECTOR_BASE + 0)) < 0 ||
            t_set_queue_vector(t, 0,
                (uint16_t)(VNET_MSI_VECTOR_BASE + 1)) < 0 ||
            t_set_queue_vector(t, 1,
                (uint16_t)(VNET_MSI_VECTOR_BASE + 2)) < 0)
        {
            t->msix_enabled = 0;
        }
    }

    vnet_queue_enable(n, 0);
    vnet_queue_enable(n, 1);
    if (n->cvq.base) vnet_queue_enable(n, 2);

    /* Buffer pools. */
    n->rx_buf_len = n->mrg ? VNET_RX_BUF_MRG :
        (uint32_t)ALIGN_UP((uint32_t)n->hdr_len + 14u + 4u +
                           (n->mtu > 1500 ? n->mtu : 1500), 16);
    n->rx_nbuf = (n->mrg || n->any_layout) ? n->rxq.num : (uint16_t)(n->rxq.num / 2);
    n->rx_pool = (uint8_t *)vnet_dma_alloc((size_t)n->rx_nbuf * n->rx_buf_len,
                                           16, &n->rx_pool_dma);
    n->tx_hdr = (uint8_t *)vnet_dma_alloc((size_t)n->txq.num * 16, 16,
                                          &n->tx_hdr_dma);
    n->tx_req = (struct tx_req *)kmalloc(sizeof(struct tx_req) * n->txq.num);
    n->tx_free = (uint16_t *)kmalloc(sizeof(uint16_t) * n->txq.num);
    n->ctrl_buf = (uint8_t *)vnet_dma_alloc(64, 16, &n->ctrl_dma);
    if (!n->rx_pool || !n->tx_hdr || !n->tx_req || !n->tx_free || !n->ctrl_buf)
    { return -1; }

    for (uint16_t i = 0; i < n->txq.num; i++) n->tx_free[i] = i;
    n->tx_nfree = n->txq.num;
    n->tx_stopped = 0;
    /* ctrl_lock zero-initialised by struct memset. */

    /* Install interrupt handlers (device still stopped; no events lost). */
    if (!t->msix_enabled)
    {
        /* INTx: route PCI IRQ via IOAPIC and register handler. */
        uint8_t irq = t->pdev->irq;
        if (irq && irq != 0xFF)
        {
            ioapic_set_entry(irq, (uint8_t)(VNET_MSI_VECTOR_BASE + 3), 0, 0);
            hal_irq_register((uint8_t)(VNET_MSI_VECTOR_BASE + 3),
                             vnet_irq_intx, n);
        }
    }

    /* S7: DRIVER_OK — device goes live. */
    st |= VIRTIO_STATUS_DRIVER_OK;
    t_set_status(t, st);

    vnet_rx_fill(n);  /* first doorbell happens after DRIVER_OK */
    return 0;
}

static void vnet_free_pools(struct vnet *n)
{
    if (n->rx_pool) vnet_dma_free(n->rx_pool, (size_t)n->rx_nbuf * n->rx_buf_len,
                                  n->rx_pool_dma);
    if (n->tx_hdr) vnet_dma_free(n->tx_hdr, (size_t)n->txq.num * 16, n->tx_hdr_dma);
    kfree(n->tx_req); kfree(n->tx_free);
    if (n->ctrl_buf) vnet_dma_free(n->ctrl_buf, 64, n->ctrl_dma);
    n->rx_pool = NULL; n->tx_hdr = NULL;
    n->tx_req = NULL; n->tx_free = NULL; n->ctrl_buf = NULL;
}

/* Teardown: stop device, quiesce IRQs, drain Tx, free all DMA memory. */
static void vnet_teardown(struct vnet *n)
{
    struct vnet_transport *t = &n->tp;
    struct pci_device *p = t->pdev;

    netdev_stop_queue(&n->netdev);

    /* Stop the device. */
    if (t_reset(t) < 0)
    {
        uint32_t cmd = pci_read32(p->bus, p->dev, p->func, PCI_COMMAND);
        pci_write32(p->bus, p->dev, p->func, PCI_COMMAND,
                    (uint32_t)(cmd & ~PCI_COMMAND_BUSMASTER));
    }

    /* Quiesce interrupt handlers. */
    if (t->msix_enabled)
    {
        hal_irq_unregister((uint8_t)(VNET_MSI_VECTOR_BASE + 0));
        hal_irq_unregister((uint8_t)(VNET_MSI_VECTOR_BASE + 1));
        hal_irq_unregister((uint8_t)(VNET_MSI_VECTOR_BASE + 2));
    }
    else
    {
        uint8_t irq = t->pdev->irq;
        if (irq && irq != 0xFF)
            hal_irq_unregister((uint8_t)(VNET_MSI_VECTOR_BASE + 3));
    }

    /* Complete every Tx still owned by the device. */
    for (uint16_t i = 0; i < n->txq.num; i++)
    {
        if (n->txq.cookie && n->txq.cookie[i])
        {
            struct tx_req *r = (struct tx_req *)n->txq.cookie[i];
            n->txq.cookie[i] = NULL;
            if (r->tx.mbuf) mbuf_free(r->tx.mbuf);
        }
    }

    for (uint16_t qi = 0; qi < 3; qi++) vnet_queue_disable(n, qi);
    if (n->cvq.base) vq_free_mem(&n->cvq);
    if (n->txq.base) vq_free_mem(&n->txq);
    if (n->rxq.base) vq_free_mem(&n->rxq);
    vnet_free_pools(n);

    /* Bus master off. */
    uint32_t cmd = pci_read32(p->bus, p->dev, p->func, PCI_COMMAND);
    pci_write32(p->bus, p->dev, p->func, PCI_COMMAND,
                (uint32_t)(cmd & ~PCI_COMMAND_BUSMASTER));
}

static void vnet_remove(struct vnet *n)
{
    vnet_teardown(n);
}

/* ---------------------------------------------------------------- *
 * Poll entry point (registered with netdev_register_poll)            *
 * ---------------------------------------------------------------- */

static void vnet_poll_one(struct vnet *n)
{
    unsigned long flags;
    /* Reclaim TX completions. */
    flags = spin_lock_irq(&n->txq.lock);
    vnet_tx_reclaim_locked(n);
    spin_unlock_irq(&n->txq.lock, flags);

    /* Drain RX. */
    vnet_rx_poll(n, 16);

    /* Re-arm callbacks if nothing pending (NAPI-style complete). */
    uint16_t snap_rx;
    flags = spin_lock_irq(&n->rxq.lock);
    snap_rx = vq_enable_cb_prepare(&n->rxq);
    if (vq_has_pending(&n->rxq, snap_rx))
    {
        vq_disable_cb(&n->rxq);
        spin_unlock_irq(&n->rxq.lock, flags);
    }
    else
        spin_unlock_irq(&n->rxq.lock, flags);

    uint16_t snap_tx;
    flags = spin_lock_irq(&n->txq.lock);
    snap_tx = vq_enable_cb_delayed(&n->txq);
    if (vq_has_pending(&n->txq, snap_tx))
    {
        vq_disable_cb(&n->txq);
        spin_unlock_irq(&n->txq.lock, flags);
    }
    else
        spin_unlock_irq(&n->txq.lock, flags);
}

static void vnet_poll_all(void)
{
    for (struct vnet *n = g_vnet_list; n; n = n->next)
    {
        vnet_poll_one(n);
        vnet_work(n);
    }
}

/* ---------------------------------------------------------------- *
 * Link / config change work                                          *
 * ---------------------------------------------------------------- */

static void vnet_work(struct vnet *n)
{
    if (n->cfg_changed)
    {
        n->cfg_changed = 0;
        uint16_t stcfg = 1;
        if (n->features & VNET_F_STATUS)
        {
            t_cfg_read_dev(&n->tp, 6, &stcfg, 2);
            stcfg = le16_to_cpu(stcfg);
            n->link_up = (stcfg & 1) ? 1 : 0;
        }
        if ((t_get_status(&n->tp) & VIRTIO_STATUS_NEEDS_RESET) ||
            n->reset_pending)
        {
            n->reset_pending = 0;
            vnet_teardown(n);
            struct pci_device *p = n->tp.pdev;
            uint32_t cmd = pci_read32(p->bus, p->dev, p->func, PCI_COMMAND);
            pci_write32(p->bus, p->dev, p->func, PCI_COMMAND,
                        (uint32_t)(cmd | PCI_COMMAND_BUSMASTER));
            if (vnet_bringup(n) == 0) netdev_wake_queue(&n->netdev);
        }
    }
}

/* ---------------------------------------------------------------- *
 * PCI probe                                                          *
 * ---------------------------------------------------------------- */

static int virtio_net_probe(struct pci_device *pdev)
{
    struct vnet *n = (struct vnet *)kmalloc(sizeof(*n));
    if (!n) return -1;
    memset(n, 0, sizeof(*n));
    n->tp.pdev = pdev;
    n->link_up = 1;

    if (t_discover(&n->tp) < 0) { kfree(n); return -1; }

    /* Enable PCI command bits: IO (legacy), MEM (modern), bus master. */
    uint32_t cmd = pci_read32(pdev->bus, pdev->dev, pdev->func, PCI_COMMAND);
    cmd |= PCI_COMMAND_MEMORY | PCI_COMMAND_BUSMASTER;
    if (!n->tp.modern) cmd |= PCI_COMMAND_IO;
    pci_write32(pdev->bus, pdev->dev, pdev->func, PCI_COMMAND, cmd);

    /* netdev setup. */
    struct netdev *nd = &n->netdev;
    memset(nd, 0, sizeof(*nd));
    strcpy(nd->name, "eth0");
    nd->mtu = 1500;
    nd->tx = vnet_netdev_tx;
    nd->priv = n;

    if (vnet_bringup(n) < 0)
    {
        vnet_teardown(n);
        kfree(n);
        return -1;
    }

    /* mac already read in bringup; copy to netdev. */
    memcpy(nd->mac, n->mac, 6);
    nd->mtu = n->mtu;

    netdev_register(&n->netdev);
    netdev_register_poll(vnet_poll_all);
    dhcp_start(&n->netdev);

    n->next = g_vnet_list;
    g_vnet_list = n;
    printk("virtio-net: %s bound (modern=%d msi-x=%d mtu=%u)\n",
           n->tp.modern ? "modern" : "legacy", n->tp.modern,
           n->tp.msix_enabled, n->mtu);
    return 0;
}

static struct driver virtio_net_driver = {
    .name        = "virtio-net",
    .vendor      = VIRTIO_VENDOR_ID,
    .device      = 0xFFFFu,   /* match any 0x1AF4 device; probe validates net */
    .pci_class   = 0xFFu,
    .pci_subclass= 0xFFu,
    .probe       = virtio_net_probe,
};

void virtio_net_driver_init(void)
{
    driver_register(&virtio_net_driver);
}
