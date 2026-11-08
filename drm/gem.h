#ifndef AXIOME_DRM_GEM_H
#define AXIOME_DRM_GEM_H

#include "drm/drm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GEM buffer objects: refcounted, physically contiguous, mmap-able.
   Backed by pmm frames (identity-mapped) like the existing dri.c. */

/* Allocate a GEM object of `size` bytes. Returns NULL on failure. */
struct drm_gem_object *drm_gem_alloc(struct drm_device *dev, uint64_t size);

/* Free a GEM object (decrements refcount, frees when zero). */
void drm_gem_free(struct drm_gem_object *gem);

/* mmap a GEM object into the current process's address space.
   Returns the virtual address, or NULL on failure. */
void *drm_gem_mmap(struct drm_gem_object *gem, uint64_t offset, size_t len);

/* Look up a GEM object by handle from the device's table. */
struct drm_gem_object *drm_gem_lookup_handle(struct drm_device *dev, uint32_t handle);

#ifdef __cplusplus
}
#endif

#endif
