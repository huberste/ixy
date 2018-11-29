#include <fcntl.h>
#include <libgen.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <linux/vfio.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>

static int vfio_cfd;
static uint64_t _iova = 0;

/* Convert virtual address to IOVA */
static uint64_t get_iova(uint32_t size) {
    uint64_t ret = _iova;
    _iova += size;
    return ret;
}

/* returns zero on success or -1 else */
int bind_pci_device_to_vfio(char* pci_addr) {
    return -1;
}

/* returns the devices file descriptor or -1 on error */
int vfio_init(char* pci_addr) {
    // find iommu group for the device
	// `readlink /sys/bus/pci/device/<segn:busn:devn.funcn>/iommu_group`
	char path[128], iommu_group_path[128];
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
		 // No iommu_group for device
		return -1;
	}

	iommu_group_path[len] = '\0';
	char* group_name = basename(iommu_group_path);
	int groupid;
	ret = sscanf(group_name, "%d", &groupid);
	if(ret != 1){
		// Unkonwn group
		return -1;
	}

	// open vfio file to create new cfio conainer
	vfio_cfd = open("/dev/vfio/vfio", O_RDWR);
	if(vfio_cfd < 0){
		// Failed to open /dev/vfio/vfio
		return -1;
	}

	// open VFIO Group containing the device
	snprintf(path, sizeof(path), "/dev/vfio/%d", groupid);
	int vfio_gfd = open(path, O_RDWR);
	if(vfio_gfd < 0){
		// Failed to open vfio group
		return -1;
	}

	// check if group is viable
	struct vfio_group_status group_status = { .argsz = sizeof(group_status) };
	ret = ioctl(vfio_gfd, VFIO_GROUP_GET_STATUS, &group_status);
	if(ret == -1) {
		return ret;
	}
	if(!group_status.flags & VFIO_GROUP_FLAGS_VIABLE){
		// VFIO group is not viable
		return -1;
	}

	// Add device to container
	ret = ioctl(vfio_gfd, VFIO_GROUP_SET_CONTAINER, vfio_cfd);
	if(ret == -1){
		// Failed to set container
		return -1;
	}

	// set vfio type (type1 is for IOMMU like VT-d or AMD-Vi)
	ret = ioctl(vfio_cfd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU);
	if(ret == -1){
		// Failed to set iommu type
		return -1;
	}

	// get device descriptor
	int vfio_fd = ioctl(vfio_gfd, VFIO_GROUP_GET_DEVICE_FD, pci_addr);
	if(vfio_fd < 0){
		// Cannot get device fd
		return -1;
	}

	return 0;
}

/* returns a uint8_t pointer to the MMAPED region or MAP_FAILED if failed */
uint8_t* vfio_map_resource(int vfio_fd, int region_index) {
	struct vfio_region_info region_info = { .argsz = sizeof(region_info) };
	region_info.index = region_index;
	int ret = ioctl(vfio_fd, VFIO_DEVICE_GET_REGION_INFO, &region_info);
    if(ret == -1){
		// Failed to set iommu type
		return MAP_FAILED;
	}
	return (uint8_t*) mmap(NULL, region_info.size, PROT_READ | PROT_WRITE, MAP_SHARED, vfio_fd, region_info.offset);
}

/* returns iova (physical address of the DMA memory from device view) on success
 * or -1 else */
uint64_t vfio_map_dma(int fd, uint64_t vaddr, uint32_t size) {
    uint64_t iova = get_iova(size);
    struct vfio_iommu_type1_dma_map dma_map = {
		.vaddr = vaddr,
		.iova = iova,
		.size = size,
		.argsz = sizeof(dma_map),
		.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE};
	int ret = ioctl(vfio_cfd, VFIO_IOMMU_MAP_DMA, &dma_map);
    if(ret == -1){
		// Failed to map DMA region
		return -1;
	}
    return iova;
}

/* unmaps previously mapped DMA region. returns 0 on success */
uint64_t vfio_unmap_dma(int fd, uint64_t iova, uint32_t size) {
struct vfio_iommu_type1_dma_unmap  dma_unmap = {
        .argsz = sizeof(dma_unmap),
        .flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
        .iova = iova,
        .size = size};
    int ret = ioctl(vfio_cfd, VFIO_IOMMU_UNMAP_DMA, &dma_unmap);
    if(ret == -1){
        // Failed to unmap DMA region
        return -1;
    }
    return ret;
}