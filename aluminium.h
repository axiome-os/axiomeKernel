#ifndef AXIOME_KERNEL_ALUMINIUM_H
#define AXIOME_KERNEL_ALUMINIUM_H

#include <stdint.h>
#include <stddef.h>

/* Kernel-side ABI for the Aluminium graphics API.
 *
 * Userspace (libaluminium) talks to /devices/aluminium0 through these
 * ioctls. Command buffers are GEM buffer objects mapped CPU-visible; the
 * application records AlCmd structures into the mapped BO and submits it
 * for execution.
 *
 * This header is shared between kernel C code and userspace (via the
 * aluminium client library). Keep it freestanding (stdint only).
 */

/* Device path. */
#define ALUMINIUM_DEV_PATH "/devices/aluminium0"

/* IOCTL numbers — our own namespace, not Linux DRM. */
#define ALUMINIUM_IOCTL_BASE          0xA0
#define ALUMINIUM_IOCTL_CREATE_DEVICE (ALUMINIUM_IOCTL_BASE + 0x00)
#define ALUMINIUM_IOCTL_CREATE_QUEUE  (ALUMINIUM_IOCTL_BASE + 0x01)
#define ALUMINIUM_IOCTL_GEM_CREATE    (ALUMINIUM_IOCTL_BASE + 0x02)
#define ALUMINIUM_IOCTL_GEM_MMAP      (ALUMINIUM_IOCTL_BASE + 0x03)
#define ALUMINIUM_IOCTL_GEM_FREE      (ALUMINIUM_IOCTL_BASE + 0x04)
#define ALUMINIUM_IOCTL_SUBMIT        (ALUMINIUM_IOCTL_BASE + 0x05)
#define ALUMINIUM_IOCTL_PRESENT       (ALUMINIUM_IOCTL_BASE + 0x06)
#define ALUMINIUM_IOCTL_GET_MODE      (ALUMINIUM_IOCTL_BASE + 0x07)
#define ALUMINIUM_IOCTL_GET_CAPS      (ALUMINIUM_IOCTL_BASE + 0x08)
#define ALUMINIUM_IOCTL_FENCE_WAIT    (ALUMINIUM_IOCTL_BASE + 0x09)
#define ALUMINIUM_IOCTL_FENCE_SIGNAL  (ALUMINIUM_IOCTL_BASE + 0x0A)

#define ALUMINIUM_IOCTL_CMD_SIZE 32

/* Command opcodes — one per command-buffer entry. */
enum aluminium_cmd_op {
    ALUMINIUM_CMD_NOP          = 0,
    ALUMINIUM_CMD_FILL_RECT    = 1,
    ALUMINIUM_CMD_BLIT         = 2,
    ALUMINIUM_CMD_CLEAR        = 3,
    ALUMINIUM_CMD_PRESENT      = 4,
};

/* Fixed-size command written into a mapped GEM BO. */
struct aluminium_cmd {
    uint32_t op;
    uint32_t args[7];
};

/* IOCTL argument structs (copied in/out by the kernel). */

struct aluminium_gem_create {
    uint64_t size;     /* in */
    uint32_t handle;   /* out */
    uint32_t pad;
};

struct aluminium_gem_mmap {
    uint32_t handle;   /* in */
    uint32_t pad;
    uint64_t offset;   /* out: (handle << 32) | byte_off, passed to mmap() */
    uint64_t size;     /* out */
};

struct aluminium_submit {
    uint32_t gem_handle;   /* GEM BO holding the command buffer */
    uint32_t cmd_count;    /* number of aluminium_cmd entries */
    uint32_t fence;        /* fence to signal on completion */
    uint32_t pad;
};

struct aluminium_present {
    uint32_t gem_handle;   /* GEM BO to present */
    uint32_t x, y, w, h;   /* dirty rect */
};

struct aluminium_get_mode {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t format;
};

struct aluminium_caps {
    uint64_t caps;
    uint32_t detail;
    uint32_t pad;
};

void aluminium_init(void);

#endif /* AXIOME_KERNEL_ALUMINIUM_H */
