#ifndef AXIOME_DEVFS_H
#define AXIOME_DEVFS_H

#include "driver.h"

struct dev_node {
    int kind;
    struct device *dev;
};

#endif
