#ifdef USE_VFIO

#include "vfio.h"
#include "log.h"
#include "memory.h"
#include "driver/device.h"

#include <fcntl.h>
#include <libgen.h>
#include <stdlib.h>
#include <string.h>
#include <asm/errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <linux/vfio.h>

int vfio_init(struct ixy_device* dev){
	debug("Initialize vfio device");
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
				error("Bad file descriptor.");
				break;
			case EFAULT:
				error("argp references an inaccessible memory area.");
				break;
			case EINVAL:
				error("request or argp is not valid.");
				break;
			case ENOTTY:
				error("fd is no tty.");
				break;
			default:
				error("unknown error...");
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

	return 0;
}

void vfio_enable_dma(struct ixy_device* dev) {
	// write to the command register (offset 4) in the PCIe config space
	int command_register_offset = 4;
	// bit 2 is "bus master enable", see PCIe 3.0 specification section 7.5.1.1
	int bus_master_enable_bit = 2;
	// Get region info for config region
	struct vfio_region_info conf_reg = { .argsz = sizeof(conf_reg) };
	conf_reg.index = VFIO_PCI_CONFIG_REGION_INDEX;
	check_err(ioctl(dev->vfio_fd, VFIO_DEVICE_GET_REGION_INFO, &conf_reg), "get vfio config region info");
	uint16_t dma = 0;
	pread(dev->vfio_fd, &dma, 2, conf_reg.offset + command_register_offset);
	dma |= 1 << bus_master_enable_bit;
	pwrite(dev->vfio_fd, &dma, 2, conf_reg.offset + command_register_offset);
}

uint8_t* vfio_map_resource(struct ixy_device* dev){
	vfio_enable_dma(dev);
	// Get region info for BAR0
	struct vfio_region_info bar0_reg = { .argsz = sizeof(bar0_reg) };
	bar0_reg.index = VFIO_PCI_BAR0_REGION_INDEX;
	check_err(ioctl(dev->vfio_fd, VFIO_DEVICE_GET_REGION_INFO, &bar0_reg), "get vfio BAR0 region info");
	return (uint8_t*) check_err(mmap(NULL, bar0_reg.size, PROT_READ | PROT_WRITE, MAP_SHARED, dev->vfio_fd, bar0_reg.offset), "mmap VFIO pci resource");
}

/* This function is not needed at the moment, it's function is implemented in memory.c/memory_allocate_dma */
struct dma_memory vfio_allocate_dma(struct ixy_device* dev, size_t size, bool require_contiguous) {
	/* old memoy_allocate_dma function
	// round up to multiples of 2 MB if necessary, this is the wasteful part
	// this could be fixed by co-locating allocations on the same page until a request would be too large
	// when fixing this: make sure to align on 128 byte boundaries (82599 dma requirement)
	if (size % HUGE_PAGE_SIZE) {
		size = ((size >> HUGE_PAGE_BITS) + 1) << HUGE_PAGE_BITS;
	}
	if (require_contiguous && size > HUGE_PAGE_SIZE) {
		// this is the place to implement larger contiguous physical mappings if that's ever needed
		error("could not map physically contiguous memory");
	}
	// unique filename, C11 stdatomic.h requires a too recent gcc, we want to support gcc 4.8
	uint32_t id = __sync_fetch_and_add(&huge_pg_id, 1);
	char path[PATH_MAX];
	snprintf(path, PATH_MAX, "/mnt/huge/ixy-%d-%d", getpid(), id);
	// temporary file, will be deleted to prevent leaks of persistent pages
	int fd = check_err(open(path, O_CREAT | O_RDWR, S_IRWXU), "open hugetlbfs file, check that /mnt/huge is mounted");
	check_err(ftruncate(fd, (off_t) size), "allocate huge page memory, check hugetlbfs configuration");
	void* virt_addr = (void*) check_err(mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_HUGETLB, fd, 0), "mmap hugepage");
	// never swap out DMA memory
	check_err(mlock(virt_addr, size), "disable swap for DMA memory");
	// don't keep it around in the hugetlbfs
	close(fd);
	unlink(path);
#ifdef USE_VFIO
	// create IOMMU mapping
	for(uint32_t i = 0; i < size / HUGE_PAGE_SIZE; i++){
		void* addr = virt_addr + HUGE_PAGE_SIZE*i;
		uint64_t vaddr = (uint64_t)addr;
		uint64_t iova = (uint64_t)virt_to_phys(addr);
		check_err(vfio_map_dma(dev, vaddr, iova, HUGE_PAGE_SIZE), "create IOMMU mapping");
	}
#endif
	return (struct dma_memory) {
		.virt = virt_addr,
		.phy = virt_to_phys(virt_addr)
	};
	*/

	// Allocate some space and setup a DMA mapping
	struct vfio_iommu_type1_dma_map dma_map = { .argsz = sizeof(dma_map) };
	dma_map.vaddr = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
	dma_map.size = size;
	// The following obviosly won't work when more than one DMA map is needed!
	// dma_map.iova = 0; // starting at 0x0 from device view
	// TODO(stefan.huber@stusta.de): use other, more efficient mapping?
	dma_map.iova = dma_map.vaddr; // starting at wherever the vaddr starts. == Identity Map
	dma_map.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;

	int result = ioctl(container, VFIO_IOMMU_MAP_DMA, &dma_map);
	if(result == -1) { // ioctl can return values > 0 and it is no error!
		error("Could not ioctl VFIO_IOMMU_MAP_DMA: errno = 0x%x:", errno);
		switch(errno) {
			case EBADF:
					error("Bad file descriptor.");
					break;
			case EFAULT:
					error("argp references an inaccessible memory area.");
					break;
			case EINVAL:
					error("request or argp is not valid.");
					break;
			case ENOTTY:
					error("fd is no tty.");
					break;
			default:
					error("unknown error...");
		}
	}
	return (struct dma_memory) {
			.virt = dma_map.vaddr,
			.phy = dma_map.iova
	};
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
