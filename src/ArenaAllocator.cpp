//
// Created by znix on 10/07/2022.
//

#include "ArenaAllocator.h"
#include "common/Platform.h"
#include "fmt/format.h"

namespace mm = mem_management;

ArenaAllocator::ArenaAllocator() { m_pageSize = mm::getPageSize(); }

ArenaAllocator::~ArenaAllocator() {
	for (const MappedBlock &block : m_blocks) {
		if (!mm::deallocateMemory(block.addr, block.size)) {
			fmt::print(stderr, "Failed to deallocate block in arena allocator: {} {}\n", errno, strerror(errno));
			abort();
		}
	}
	m_blocks.clear();
}

void *ArenaAllocator::AllocateSlowPath(int size) {
	// Grow the number of pages we allocate as more memory is used, to reduce the number of mmaps required (and improve
	// wastage at the end of blocks if larger objects are used) but not waste lots of memory on small arenas.
	int blockSize = m_pageSize;
	if (!m_blocks.empty())
		blockSize = m_blocks.back().size * 2;

	// If the block wouldn't be able to contain the object, grow to that size and make sure we're aligned to
	// the page size.
	if (blockSize < size) {
		blockSize = size;
	}
	int overhang = blockSize % m_pageSize;
	if (overhang) {
		blockSize += m_pageSize - overhang;
	}

	// Allocate fresh page[s] from the kernel
	void *addr = mm::allocateMemory(blockSize);
	if (addr == nullptr) {
		// TODO unify error handling
#ifdef _WIN32
		std::string error = plat_util::getLastWindowsError();
		fmt::print("Failed to allocate memory block in arena allocator, size {}. Error: {}\n", blockSize,
		    error.c_str());
#else
		fmt::print("Failed to allocate memory block in arena allocator, size {}. Error {} {}\n", blockSize, errno,
		    strerror(errno));
#endif
		abort();
	}

	// Insert this to the page list
	m_blocks.emplace_back(MappedBlock{
	    .addr = addr,
	    .size = blockSize,
	});

	// Make this the page the fast-path allocation function will use
	m_currentPage = addr;
	m_currentPageOffset = 0;
	m_currentPageRemaining = blockSize;

	// Start at the beginning of the page
	return addr;
}
