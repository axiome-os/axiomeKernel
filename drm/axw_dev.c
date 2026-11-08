/* kernel/drm/axw_dev.c — /devices/axw-0 display protocol device.
   Multiplexes axw protocol messages between the display server (compositor)
   and GUI clients. Each open() creates a connection; the first opener
   becomes the server (compositor). read()/write() carry axw_msg_hdr frames.
   Per-connection state is stored in g_conns[] and associated with the fd
   via struct vfs_file.priv (set by syscall.c sys_open). */

#include "drm/axw_dev.h"
#include "userspace/libaxw/axw-protocol.h"
#include "drm/drm_ioctl.h"
#include "driver.h"
#include "slab.h"
#include "printk.h"
#include "spinlock.h"
#include "string.h"
#include "sched.h"
#include "vfs.h"
#include <stddef.h>

#define AXW_MAX_CONNS 16
#define AXW_QUEUE_DEPTH 64

struct axw_conn {
    int used;
    int is_server;          /* first opener = compositor */
    uint32_t id;            /* connection id (1-based) */
    struct axw_msg_hdr rxq[AXW_QUEUE_DEPTH];
    uint32_t rx_args[AXW_QUEUE_DEPTH][AXW_MAX_ARGS];
    uint8_t rx_payload[AXW_QUEUE_DEPTH][AXW_MAX_ARGS * 4];
    int rx_head, rx_tail, rx_count;
    spinlock_t lock;
};

static struct axw_conn g_conns[AXW_MAX_CONNS];
static int g_server_conn = -1; /* index into g_conns */

static struct axw_conn *conn_at(int index)
{
    if (index < 0 || index >= AXW_MAX_CONNS)
        return 0;
    struct axw_conn *c = &g_conns[index];
    return c->used ? c : 0;
}

struct axw_conn *axw_dev_conn_at(int index)
{
    return conn_at(index);
}

long axw_dev_read_conn(struct axw_conn *c, void *buf, size_t len)
{
    if (!c || !buf || len == 0)
        return -1;
    unsigned long flags = spin_lock_irq(&c->lock);
    if (c->rx_count == 0)
    {
        spin_unlock_irq(&c->lock, flags);
        return 0; /* empty, non-blocking */
    }
    int idx = c->rx_tail;
    const struct axw_msg_hdr *h = &c->rxq[idx];
    uint16_t total = h->size;
    if (total > (uint16_t)len)
        total = (uint16_t)len;
    __builtin_memcpy(buf, h, total);
    size_t args_size = (size_t)(h->size - 8u);
    if (args_size > 0 && total > 8u)
    {
        size_t copy = args_size < (size_t)(total - 8u) ? args_size : (size_t)(total - 8u);
        __builtin_memcpy((uint8_t *)buf + 8, c->rx_args[idx], copy);
    }
    c->rx_tail = (c->rx_tail + 1) % AXW_QUEUE_DEPTH;
    c->rx_count--;
    spin_unlock_irq(&c->lock, flags);
    return (long)total;
}

long axw_dev_write_conn(struct axw_conn *c, const void *buf, size_t len)
{
    if (!c || !buf || len < sizeof(struct axw_msg_hdr))
        return -1;
    const struct axw_msg_hdr *h = (const struct axw_msg_hdr *)buf;
    uint16_t total = h->size;
    if (total < 8u || total > (uint16_t)len)
        return -1;

    if (c->is_server)
    {
        /* Server broadcasting: deliver to all client connections */
        int delivered = 0;
        for (int i = 0; i < AXW_MAX_CONNS; i++)
        {
            struct axw_conn *t = conn_at(i);
            if (!t || t->is_server)
                continue;
            unsigned long f2 = spin_lock_irq(&t->lock);
            if (t->rx_count < AXW_QUEUE_DEPTH)
            {
                int idx = (t->rx_head + t->rx_count) % AXW_QUEUE_DEPTH;
                __builtin_memcpy(&t->rxq[idx], h, sizeof(*h));
                size_t args_size = (size_t)(total - 8u);
                if (args_size > 0)
                    __builtin_memcpy(t->rx_args[idx], (const uint8_t *)buf + 8,
                                     args_size);
                t->rx_count++;
                delivered++;
            }
            spin_unlock_irq(&t->lock, f2);
        }
        return delivered > 0 ? (long)total : -1;
    }
    else
    {
        /* Client -> server: deliver to server connection */
        if (g_server_conn < 0)
            return -1;
        struct axw_conn *srv = conn_at(g_server_conn);
        if (!srv)
            return -1;
        unsigned long f2 = spin_lock_irq(&srv->lock);
        if (srv->rx_count < AXW_QUEUE_DEPTH)
        {
            int idx = (srv->rx_head + srv->rx_count) % AXW_QUEUE_DEPTH;
            __builtin_memcpy(&srv->rxq[idx], h, sizeof(*h));
            size_t args_size = (size_t)(total - 8u);
            if (args_size > 0)
                __builtin_memcpy(srv->rx_args[idx], (const uint8_t *)buf + 8,
                                 args_size);
            /* Keep the sender_id set by the client (AXW_ID_SHM, AXW_ID_COMPOSITOR, etc.) */
            srv->rx_count++;
            spin_unlock_irq(&srv->lock, f2);
            return (long)total;
        }
        spin_unlock_irq(&srv->lock, f2);
        return -1;
    }
}

int axw_dev_open_conn(void)
{
    for (int i = 0; i < AXW_MAX_CONNS; i++)
    {
        if (!g_conns[i].used)
        {
            struct axw_conn *c = &g_conns[i];
            __builtin_memset(c, 0, sizeof(*c));
            c->used = 1;
            c->id = (uint32_t)(i + 1);
            c->rx_head = 0;
            c->rx_tail = 0;
            c->rx_count = 0;
            c->lock.locked = 0;
            if (g_server_conn < 0)
            {
                c->is_server = 1;
                g_server_conn = i;
                printk("AXW: connection %u became server\n", c->id);
            }
            else
            {
                c->is_server = 0;
                printk("AXW: connection %u (client)\n", c->id);
            }
            return i;
        }
    }
    return -1;
}

void axw_dev_close_conn(int conn_index)
{
    struct axw_conn *c = conn_at(conn_index);
    if (!c)
        return;
    if (c->is_server && g_server_conn == conn_index)
    {
        g_server_conn = -1;
    }
    __builtin_memset(c, 0, sizeof(*c));
}

int axw_dev_server_id(void)
{
    return g_server_conn;
}

static long axw_dev_read(struct device *d, uint64_t off, void *buf, size_t len)
{
    (void)d; (void)off;
    /* The caller (syscall.c) passes the conn via a side channel.
       For now, this is unused; routing is done in sys_read/sys_write. */
    return -1;
}

static long axw_dev_write(struct device *d, uint64_t off,
                          const void *buf, size_t len)
{
    (void)d; (void)off;
    return -1;
}

static long axw_dev_mmap(struct device *d, uint64_t off, uint64_t virt,
                         size_t len, uint64_t flags)
{
    (void)d; (void)off; (void)virt; (void)len; (void)flags;
    return -1;
}

static struct dev_ops axw_ops = {
    .read  = axw_dev_read,
    .write = axw_dev_write,
    .mmap  = axw_dev_mmap,
};

void axw_dev_init(void)
{
    if (device_find("axw0"))
        return;
    struct device *d = (struct device *)kmalloc(sizeof(struct device));
    if (!d)
        return;
    __builtin_memset(d, 0, sizeof(*d));
    const char *n = "axw0";
    for (int i = 0; n[i] && i < 31; i++)
        d->name[i] = n[i];
    d->major = 229;
    d->minor = 0;
    d->type = DEV_CHAR;
    d->ops = axw_ops;
    device_register(d);
    printk("DRV: axw0 -> /devices/axw0 (axw display protocol)\n");
}
