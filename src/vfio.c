#ifdef USE_VFIO

#include "vfio.h"
#include "log.h"
#include "driver/device.h"

#include <fcntl.h>
#include <libgen.h>
#include <stdlib.h>
#include <string.h>
#include <asm/errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <linux/vfio.h>

int vfio_init(struct ixy_device* dev){
	debug("Initialize vfio");
	// find iommu group for the device
	// `readlink /sys/bus/pci/device/<segn:busn:devn.funcn>/iommu_group`
	char path[128], iommu_group_path[128];
	struct stat st;
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/", dev->pci_addr);
	int ret = stat(path, &st);
	if(ret < 0){
		warn("No such device: %s", path);
		return -1;
	}
	strncat(path, "iommu_group", sizeof(path) - strlen(path) - 1);

	int len = readlink(path, iommu_group_path, sizeof(iommu_group_path));
	if(len <= 0){
		warn("No iommu_group for device");
		return -1;
	}

	iommu_group_path[len] = '\0';
	char* group_name = basename(iommu_group_path);
	int groupid;
	ret = sscanf(group_name, "%d", &groupid);
	if(ret != 1){
		warn("Unkonwn group");
		return -1;
	}

	// open vfio file to create new cfio conainer
	dev->vfio_cfd = open("/dev/vfio/vfio", O_RDWR);
	if(dev->vfio_cfd < 0){
		warn("Failed to open /dev/vfio/vfio");
		return -1;
	}

	// open VFIO Group containing the device
	snprintf(path, sizeof(path), "/dev/vfio/%d", groupid);
	dev->vfio_gfd = open(path, O_RDWR);
	if(dev->vfio_gfd < 0){
		warn("Failed to open %s", path);
		return -1;
	}

	// check if group is viable
	struct vfio_group_status group_status = { .argsz = sizeof(group_status) };
	ret = ioctl(dev->vfio_gfd, VFIO_GROUP_GET_STATUS, &group_status);
	if(ret == -1) { // ioctl can return values > 0 and it is no error!
		warn("Could not ioctl VFIO_GROUP_GET_STATUS: errno = 0x%x:", errno);
		switch(errno) {
			case EBADF:
				debug("Bad file descriptor.");
				break;
			case EFAULT:
				debug("argp references an inaccessible memory area.");
				break;
			case EINVAL:
				debug("request or argp is not valid.");
				break;
			case ENOTTY:
				debug("fd is no tty.");
				break;
			default:
				debug("unknown error...");
		}
		return ret;
	}
	if(!group_status.flags & VFIO_GROUP_FLAGS_VIABLE){
		warn("VFIO group is not viable");
		return -1;
	}

	// Add device to container
	ret = ioctl(dev->vfio_gfd, VFIO_GROUP_SET_CONTAINER, &dev->vfio_cfd);
	if(ret != 0){
		warn("Failed to set container");
		return -1;
	}

	// set vfio type (type1 is for IOMMU like VT-d or AMD-Vi)
	ret = ioctl(dev->vfio_cfd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU);
	if(ret != 0){
		warn("Failed to set iommu type");
		return -1;
	}

	// get device descriptor
	dev->vfio_fd = ioctl(dev->vfio_gfd, VFIO_GROUP_GET_DEVICE_FD, dev->pci_addr);
	if(dev->vfio_fd < 0){
		warn("Cannot get device fd");
		return -1;
	}

	// Test and setup the device
	struct vfio_device_info device_info = { .argsz = sizeof(device_info) };
	ioctl(dev->vfio_fd, VFIO_DEVICE_GET_INFO, &device_info);

	return 0;
}

int vfio_map_resource(const char* pci_addr){
	// this is what pci_map_resource does:
	/*
	char path[PATH_MAX];
	snprintf(path, PATH_MAX, "/sys/bus/pci/devices/%s/resource0", pci_addr);
	debug("Mapping PCI resource at %s", path);
	// for VFIO we probably don't want to unbind the driver...
	//remove_driver(pci_addr);
	enable_dma(pci_addr);
	int fd = check_err(open(path, O_RDWR), "open pci resource");
	struct stat stat;
	check_err(fstat(fd, &stat), "stat pci resource");
	return (uint8_t*) check_err(mmap(NULL, stat.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0), "mmap pci resource");
	*/
	// TODO(stefan.huber@stusta.de): write this procedure...
	return 0;
}

int vfio_map_dma(struct ixy_device* dev, uint64_t vaddr, uint64_t iova, uint32_t size){
	struct vfio_iommu_type1_dma_map dma_map = {
		.vaddr = vaddr,
		.iova = iova,
		.size = size,
		.argsz = sizeof(dma_map),
		.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE};
	return ioctl(dev->vfio_cfd, VFIO_IOMMU_MAP_DMA, &dma_map);
}

#endif
