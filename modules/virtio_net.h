#ifndef AXIOME_MODULES_VIRTIO_NET_H
#define AXIOME_MODULES_VIRTIO_NET_H

#include <stdint.h>
#include <stddef.h>

/*
 * virtio-net driver (OASIS Virtio 1.0 - 1.2, PCI transport, split rings).
 *
 * Implemented against this tree's PCI/MMIO/port-I/O/DMA/IRQ primitives
 * (pci.h, io.h, hal/cshim.h, pmm.h, vmm.h).  x86_64 is little-endian, so the
 * modern (little-endian on the wire) and legacy (guest-native) formats both
 * read natively; the byte-swap helpers are retained for correctness notes and
 * any future big-endian port.
 */

/* ---------------------------------------------------------------- *
 * PCI identification                                               *
 * ---------------------------------------------------------------- */

#define VIRTIO_VENDOR_ID          0x1AF4u
#define VIRTIO_DEV_ID_NET_TRANS   0x1000u   /* transitional (legacy interface) */
#define VIRTIO_DEV_ID_NET_MODERN  0x1041u   /* modern-only (0x1040 + type 1) */
#define VIRTIO_SUBSYS_DEV_NET     1u        /* virtio device type 1 = network */

/* PCI config offsets used by this driver. */
#define PCI_VENDOR_ID      0x00
#define PCI_DEVICE_ID      0x02
#define PCI_COMMAND        0x04
#define PCI_STATUS         0x06
#define PCI_REVISION_ID    0x08
#define PCI_CLASS_REV      0x08
#define PCI_BASE_ADDRESS_0 0x10
#define PCI_CARDBUS_CIS    0x28
#define PCI_SUBSYSTEM_VENDOR_ID 0x2C
#define PCI_SUBSYSTEM_ID     0x2E
#define PCI_CAPABILITY_LIST 0x34
#define PCI_INTERRUPT_LINE 0x3C
#define PCI_INTERRUPT_PIN   0x3D

#define PCI_COMMAND_IO        0x0001u
#define PCI_COMMAND_MEMORY    0x0002u
#define PCI_COMMAND_BUSMASTER 0x0004u
#define PCI_COMMAND_INTX_DISABLE 0x0400u

#define PCI_STATUS_CAP_LIST 0x0010u

/* PCI capability IDs. */
#define PCI_CAP_ID_VENDOR   0x09   /* vendor-specific (virtio modern cap) */
#define PCI_CAP_ID_MSIX     0x11

/* virtio-pci capability types (cfg_type). */
#define VIRTIO_PCI_CAP_COMMON_CFG  1
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2
#define VIRTIO_PCI_CAP_ISR_CFG     3
#define VIRTIO_PCI_CAP_DEVICE_CFG  4
#define VIRTIO_PCI_CAP_PCI_CFG     5

struct virtio_pci_cap {
    uint8_t  cap_vndr;    /* 0x09 */
    uint8_t  cap_next;
    uint8_t  cap_len;
    uint8_t  cfg_type;    /* 1 COMMON, 2 NOTIFY, 3 ISR, 4 DEVICE */
    uint8_t  bar;
    uint8_t  id;          /* disambiguator (1.1+) */
    uint8_t  padding[2];
    uint32_t offset;      /* byte offset within BAR */
    uint32_t length;      /* length in bytes */
} __attribute__((packed));  /* 16 bytes */

struct virtio_pci_notify_cap {
    struct virtio_pci_cap cap;
    uint32_t notify_off_multiplier;
} __attribute__((packed));

struct virtio_pci_common_cfg {
    uint32_t device_feature_select;  /* 0x00 W */
    uint32_t device_feature;         /* 0x04 R */
    uint32_t driver_feature_select;  /* 0x08 W */
    uint32_t driver_feature;         /* 0x0C RW */
    uint16_t config_msix_vector;     /* 0x10 RW */
    uint16_t num_queues;             /* 0x12 R */
    uint8_t  device_status;          /* 0x14 RW */
    uint8_t  config_generation;      /* 0x15 R */
    uint16_t queue_select;           /* 0x16 RW */
    uint16_t queue_size;             /* 0x18 RW */
    uint16_t queue_msix_vector;      /* 0x1A RW */
    uint16_t queue_enable;           /* 0x1C RW */
    uint16_t queue_notify_off;       /* 0x1E R */
    uint64_t queue_desc;             /* 0x20 RW */
    uint64_t queue_driver;           /* 0x28 RW */
    uint64_t queue_device;           /* 0x30 RW */
} __attribute__((packed));           /* 56 bytes used */

/* ---------------------------------------------------------------- *
 * Device status                                                    *
 * ---------------------------------------------------------------- */

#define VIRTIO_STATUS_ACK         0x01u
#define VIRTIO_STATUS_DRIVER      0x02u
#define VIRTIO_STATUS_DRIVER_OK   0x04u
#define VIRTIO_STATUS_FEATURES_OK 0x08u
#define VIRTIO_STATUS_NEEDS_RESET 0x40u
#define VIRTIO_STATUS_FAILED      0x80u

#define VIRTIO_MSI_NO_VECTOR 0xFFFFu

/* ---------------------------------------------------------------- *
 * Feature bits                                                     *
 * ---------------------------------------------------------------- */

#define VIRTIO_F_RING_INDIRECT_DESC (1ull << 28)
#define VIRTIO_F_RING_EVENT_IDX     (1ull << 29)
#define VIRTIO_F_VERSION_1          (1ull << 32)
#define VIRTIO_F_ACCESS_PLATFORM    (1ull << 33)

#define VNET_F_CSUM        (1ull << 0)
#define VNET_F_GUEST_CSUM  (1ull << 1)
#define VNET_F_MTU         (1ull << 3)
#define VNET_F_MAC         (1ull << 5)
#define VNET_F_HOST_TSO4   (1ull << 11)
#define VNET_F_HOST_TSO6   (1ull << 12)
#define VNET_F_HOST_ECN    (1ull << 13)
#define VNET_F_MRG_RXBUF   (1ull << 15)
#define VNET_F_STATUS      (1ull << 16)
#define VNET_F_CTRL_VQ     (1ull << 17)
#define VNET_F_CTRL_RX     (1ull << 18)

/* ---------------------------------------------------------------- *
 * Virtio-net configuration space                                   *
 * ---------------------------------------------------------------- */

struct virtio_net_config {
    uint8_t  mac[6];                    /* valid iff VNET_F_MAC */
    uint16_t status;                    /* valid iff VNET_F_STATUS */
    uint16_t max_virtqueue_pairs;       /* valid iff MQ */
    uint16_t mtu;                       /* valid iff VNET_F_MTU */
    uint32_t speed;                     /* iff SPEED_DUPLEX */
    uint8_t  duplex;                    /* iff SPEED_DUPLEX */
} __attribute__((packed));

#define VNET_CONFIG_STATUS_LINK_UP 1u
#define VNET_CONFIG_STATUS_ANNOUNCE 2u

/* ---------------------------------------------------------------- *
 * Net header                                                       *
 * ---------------------------------------------------------------- */

#define VNET_HDR_F_NEEDS_CSUM  1u
#define VNET_HDR_F_DATA_VALID  2u
#define VNET_HDR_F_RSC_INFO    4u

#define VNET_HDR_GSO_NONE   0u
#define VNET_HDR_GSO_TCPV4  1u
#define VNET_HDR_GSO_UDP    3u
#define VNET_HDR_GSO_TCPV6  4u
#define VNET_HDR_GSO_ECN    0x80u

struct virtio_net_hdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} __attribute__((packed));               /* 10 bytes */

struct virtio_net_hdr_mrg_rxbuf {
    struct virtio_net_hdr hdr;
    uint16_t num_buffers;
} __attribute__((packed));               /* 12 bytes */

/* ---------------------------------------------------------------- *
 * Split virtqueue                                                  *
 * ---------------------------------------------------------------- */

#define VRING_DESC_F_NEXT     1u
#define VRING_DESC_F_WRITE    2u
#define VRING_DESC_F_INDIRECT 4u

#define VRING_AVAIL_F_NO_INTERRUPT 1u
#define VRING_USED_F_NO_NOTIFY     1u

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed, aligned(16)));  /* 16 bytes */

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];                     /* [num], then used_event */
} __attribute__((packed, aligned(2)));

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed, aligned(4)));

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[];       /* [num], then avail_event */
} __attribute__((packed, aligned(4)));

/* ---------------------------------------------------------------- *
 * Driver-facing scatter/gather                                     *
 * ---------------------------------------------------------------- */

struct vnet_sg {
    uint64_t dma;        /* DMA (physical) address of the fragment */
    uint32_t len;        /* length in bytes */
};

/* ---------------------------------------------------------------- *
 * Public driver interface                                          *
 * ---------------------------------------------------------------- */

/* One netdev is registered per bound virtio-net device. */
extern void virtio_net_driver_init(void);

#endif /* AXIOME_MODULES_VIRTIO_NET_H */
