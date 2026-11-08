#ifndef AXIOME_AXW_DEV_H
#define AXIOME_AXW_DEV_H

#include <stdint.h>
#include <stddef.h>

struct axw_conn;

/* Called by syscall.c sys_read/sys_write when the fd is an axw connection. */
long axw_dev_read_conn(struct axw_conn *c, void *buf, size_t len);
long axw_dev_write_conn(struct axw_conn *c, const void *buf, size_t len);

/* Create a new axw connection (called from sys_open). Returns conn index or -1. */
int axw_dev_open_conn(void);

/* Get connection by index. */
struct axw_conn *axw_dev_conn_at(int index);

/* Called on close to clean up. */
void axw_dev_close_conn(int conn_index);

/* Get the server connection index. */
int axw_dev_server_id(void);

void axw_dev_init(void);

#endif
