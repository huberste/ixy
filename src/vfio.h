#ifndef IXY_VFIO_H
#define IXY_VFIO_H

#include "memory.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ixy_device;

int vfio_init(struct ixy_device* dev);
uint8_t* vfio_map_resource(struct ixy_device* dev);
struct dma_memory vfio_allocate_dma(struct ixy_device* dev, size_t size, bool require_contiguous);
int vfio_map_dma(struct ixy_device* dev, uint64_t vaddr, uint64_t iova, uint32_t size);

#endif //IXY_VFIO_H
