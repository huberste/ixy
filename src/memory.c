#include "memory.h"
#include "log.h"
#include "driver/device.h"

#include <stddef.h>
#include <linux/limits.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#ifdef USE_VFIO
#include "vfio.h"
#include <linux/vfio.h>
#endif

// translate a virtual address to a physical one via /proc/self/pagemap
static uintptr_t virt_to_phys(void* virt) {
	long pagesize = sysconf(_SC_PAGESIZE);
	int fd = check_err(open("/proc/self/pagemap", O_RDONLY), "getting pagemap");
	// pagemap is an array of pointers for each normal-sized page
	check_err(lseek(fd, (uintptr_t) virt / pagesize * sizeof(uintptr_t), SEEK_SET), "getting pagemap");
	uintptr_t phy = 0;
	check_err(read(fd, &phy, sizeof(phy)), "translating address");
	close(fd);
	if (!phy) {
		error("failed to translate virtual address %p to physical address", virt);
	}
	// bits 0-54 are the page number
	return (phy & 0x7fffffffffffffULL) * pagesize + ((uintptr_t) virt) % pagesize;
}

static uint32_t huge_pg_id;

// allocate memory suitable for DMA access in huge pages
// this requires hugetlbfs to be mounted at /mnt/huge
// not using anonymous hugepages because hugetlbfs can give us multiple pages with contiguous virtual addresses
// allocating anonymous pages would require manual remapping which is more annoying than handling files
struct dma_memory memory_allocate_dma(struct ixy_device* dev, size_t size, bool require_contiguous) {
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
	// TODO(stefan.huber@stusta.de): Why do we need the hugetlbfs file when we close is anyways?
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
		// TODO(stefan.huber@stusta.de): do we *want* virt_to_phys?
		// We don't *need* virt_to_phys with vfio, since we can just give the
		// device memory starting at any arbitrary position (dma_map.iova)
		uint64_t iova = (uint64_t)virt_to_phys(addr); /* iova = IO Virtual Address, so the memory starts here FROM DEVICE VIEW */
		check_err(vfio_map_dma(dev, vaddr, iova, HUGE_PAGE_SIZE), "create IOMMU mapping");
	}
#endif
	return (struct dma_memory) {
		.virt = virt_addr,
		.phy = virt_to_phys(virt_addr) /* for VFIO, this needs to point to the device view memory = IOVA! */
		// TODO(stefan.huber@stusta.de): We don't need this dma_memory struct, vfio_iommu_type1_dma_map has the same functionality and is created anyways.
	};
}

// allocate a memory pool from which DMA'able packet buffers can be allocated
// this is currently not yet thread-safe, i.e., a pool can only be used by one thread,
// this means a packet can only be sent/received by a single thread
// entry_size can be 0 to use the default
struct mempool* memory_allocate_mempool(struct ixy_device* dev, uint32_t num_entries, uint32_t entry_size) {
	entry_size = entry_size ? entry_size : 2048;
	// require entries that neatly fit into the page size, this makes the memory pool much easier
	// otherwise our base_addr + index * size formula would be wrong because we can't cross a page-boundary
	if (HUGE_PAGE_SIZE % entry_size) {
		error("entry size must be a divisor of the huge page size (%d)", HUGE_PAGE_SIZE);
	}
	struct mempool* mempool = (struct mempool*) malloc(sizeof(struct mempool) + num_entries * sizeof(uint32_t));
#ifdef USE_VFIO
	struct dma_memory mem = vfio_allocate_dma(dev, num_entries * entry_size, false);
#else
	struct dma_memory mem = memory_allocate_dma(dev, num_entries * entry_size, false);
#endif
	mempool->num_entries = num_entries;
	mempool->buf_size = entry_size;
	mempool->base_addr = mem.virt;
	mempool->free_stack_top = num_entries;
	for (uint32_t i = 0; i < num_entries; i++) {
		mempool->free_stack[i] = i;
		struct pkt_buf* buf = (struct pkt_buf*) (((uint8_t*) mempool->base_addr) + i * entry_size);
		// physical addresses are not contiguous within a pool, we need to get the mapping
		// minor optimization opportunity: this only needs to be done once per page
#ifdef USE_VFIO
		buf->buf_addr_phy = buf;
#else
		buf->buf_addr_phy = virt_to_phys(buf);
#endif
		buf->mempool_idx = i;
		buf->mempool = mempool;
		buf->size = 0;
	}
	return mempool;
}

uint32_t pkt_buf_alloc_batch(struct mempool* mempool, struct pkt_buf* bufs[], uint32_t num_bufs) {
	if (mempool->free_stack_top < num_bufs) {
		warn("memory pool %p only has %d free bufs, requested %d", mempool, mempool->free_stack_top, num_bufs);
		num_bufs = mempool->free_stack_top;
	}
	for (uint32_t i = 0; i < num_bufs; i++) {
		uint32_t entry_id = mempool->free_stack[--mempool->free_stack_top];
		bufs[i] = (struct pkt_buf*) (((uint8_t*) mempool->base_addr) + entry_id * mempool->buf_size);
	}
	return num_bufs;
}

struct pkt_buf* pkt_buf_alloc(struct mempool* mempool) {
	struct pkt_buf* buf = NULL;
	pkt_buf_alloc_batch(mempool, &buf, 1);
	return buf;
}

void pkt_buf_free(struct pkt_buf* buf) {
	struct mempool* mempool = buf->mempool;
	mempool->free_stack[mempool->free_stack_top++] = buf->mempool_idx;
}

