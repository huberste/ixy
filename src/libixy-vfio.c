#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <linux/limits.h>
#include <linux/vfio.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <driver/device.h>

ssize_t MIN_DMA_MEMORY = 4096; // we can not allocate less than page_size memory

void vfio_enable_dma(int device_fd) {
	// write to the command register (offset 4) in the PCIe config space
	int command_register_offset = 4;
	// bit 2 is "bus master enable", see PCIe 3.0 specification section 7.5.1.1
	int bus_master_enable_bit = 2;
	// Get region info for config region
	struct vfio_region_info conf_reg = { .argsz = sizeof(conf_reg) };
	conf_reg.index = VFIO_PCI_CONFIG_REGION_INDEX;
	check_err(ioctl(device_fd, VFIO_DEVICE_GET_REGION_INFO, &conf_reg), "get vfio config region info");
	uint16_t dma = 0;
	assert(pread(device_fd, &dma, 2, conf_reg.offset + command_register_offset) == 2);
	dma |= 1 << bus_master_enable_bit;
	assert(pwrite(device_fd, &dma, 2, conf_reg.offset + command_register_offset) == 2);
}

/* returns the devices file descriptor or -1 on error */
int vfio_init(char* pci_addr) {
	// find iommu group for the device
	// `readlink /sys/bus/pci/device/<segn:busn:devn.funcn>/iommu_group`
	char path[PATH_MAX], iommu_group_path[PATH_MAX];
	struct stat st;
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/", pci_addr);
	int ret = stat(path, &st);
	if(ret < 0){
		// No such device
		return -1;
	}
	strncat(path, "iommu_group", sizeof(path) - strlen(path) - 1);

	int len = readlink(path, iommu_group_path, sizeof(iommu_group_path));
	if(len <= 0){
		error("failed to find the iommu_group for device '%s'", pci_addr);
		return -1;
	}

	iommu_group_path[len] = '\0'; // append 0x00 to the string to end it
	char* group_name = basename(iommu_group_path);
	int groupid;
	ret = sscanf(group_name, "%d", &groupid);
	if(ret != 1){
		error("Failed to convert group id '%s' to int", group_name);
		return -1;
	}

	int firstsetup = 0; // Need to set up the container exactly once
	int cfd = get_vfio_container();
	if (cfd == -1) {
		firstsetup = 1;
		// open vfio file to create new cfio conainer
		cfd = open("/dev/vfio/vfio", O_RDWR);
		if(cfd < 0){
			error("Failed to open /dev/vfio/vfio");
			return -1;
		}
		set_vfio_container(cfd);

		// check if the container's API version is the same as the VFIO API's
		if (ioctl(cfd, VFIO_GET_API_VERSION) != VFIO_API_VERSION) {
			error("unknown VFIO API Version");
			return -1;
		}

		// check if type1 is supported
		if (ioctl(cfd, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU) != 1) {
			error("container doesn't support Type1 IOMMU");
			return -1;
		}
	}

	// open VFIO group containing the device
	snprintf(path, sizeof(path), "/dev/vfio/%d", groupid);
	int vfio_gfd = open(path, O_RDWR);
	if(vfio_gfd < 0){
		error("Failed to open vfio group");
		return -1;
	}

	// check if group is viable
	struct vfio_group_status group_status = { .argsz = sizeof(group_status) };
	ret = ioctl(vfio_gfd, VFIO_GROUP_GET_STATUS, &group_status);
	if(ret == -1) {
		error("failed to get VFIO group status. errno: %d", errno);
		return ret;
	}
	if(!group_status.flags & VFIO_GROUP_FLAGS_VIABLE){
		error("VFIO group is not viable - are all devices in the group bound to the VFIO driver?");
		return -1;
	}

	// Add group to container
	ret = ioctl(vfio_gfd, VFIO_GROUP_SET_CONTAINER, &cfd);
	if(ret == -1){
		error("Failed to set container. errno: %d", errno);
		return -1;
	}

	if (firstsetup != 0) {
		// Set vfio type (type1 is for IOMMU like VT-d or AMD-Vi) for the
		// container.
		// This can only be done after at least one group is in the container.
		ret = ioctl(cfd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU);
		if(ret != 0){
			error("Failed to set iommu type. errno: %d", -ret);
			return -1;
		}
	}

	// get device file descriptor
	int vfio_fd = ioctl(vfio_gfd, VFIO_GROUP_GET_DEVICE_FD, pci_addr);
	if(vfio_fd < 0){
		error("Cannot get device fd. errno: %d", -vfio_fd);
		return -1;
	}

	// enable DMA
	vfio_enable_dma(vfio_fd);

	return vfio_fd;
}

/* returns a uint8_t pointer to the MMAPED region or MAP_FAILED if failed */
uint8_t* vfio_map_region(int vfio_fd, int region_index) {
	struct vfio_region_info region_info = { .argsz = sizeof(region_info) };
	region_info.index = region_index;
	int ret = ioctl(vfio_fd, VFIO_DEVICE_GET_REGION_INFO, &region_info);
	if(ret == -1){
		// Failed to set iommu type
		return MAP_FAILED; // MAP_FAILED == ((void *) -1)
	}
	return (uint8_t*) check_err(mmap(NULL, region_info.size, PROT_READ | PROT_WRITE, MAP_SHARED, vfio_fd, region_info.offset), "mmap vfio bar0 resource");
}

/* returns iova (physical address of the DMA memory from device view) on success */
uint64_t vfio_map_dma(void *vaddr, uint32_t size) {
	uint64_t iova = (uint64_t) vaddr; // Identity mapping makes sense here because why not
	struct vfio_iommu_type1_dma_map dma_map = {
		.vaddr = (uint64_t) vaddr,
		.iova = iova,
		.size = size < MIN_DMA_MEMORY ? MIN_DMA_MEMORY : size,
		.argsz = sizeof(dma_map),
		.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE};
	debug("mapping iommu memory of size %d", dma_map.size);
	int cfd = get_vfio_container();
	check_err(ioctl(cfd, VFIO_IOMMU_MAP_DMA, &dma_map), "IOMMU Map DMA Memory");
	return iova;
}

/* unmaps previously mapped DMA region. returns 0 on success */
uint64_t vfio_unmap_dma(int fd, uint64_t iova, uint32_t size) {
	struct vfio_iommu_type1_dma_unmap  dma_unmap = {
		.argsz = sizeof(dma_unmap),
		.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
		.iova = iova,
		.size = size};
	int cfd = get_vfio_container();
	int ret = ioctl(cfd, VFIO_IOMMU_UNMAP_DMA, &dma_unmap);
	if(ret == -1){
		// Failed to unmap DMA region
		return -1;
	}
	return ret;
}
