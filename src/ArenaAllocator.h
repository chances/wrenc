//
// Created by znix on 10/07/2022.
//

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <utility>
#include <vector>

// For allocation logging
// #include <stdio.h>

class ArenaAllocator {
  public:
	ArenaAllocator();
	~ArenaAllocator();

	// Obviously not copyable
	ArenaAllocator(const ArenaAllocator &) = delete;
	ArenaAllocator &operator=(const ArenaAllocator &) = delete;

	template <typename T, typename... Args> T *New(Args &&...args) {
		void *mem = AllocateMem(sizeof(T), alignof(T));
		return new (mem) T(std::forward<Args>(args)...);
	}

	inline void *AllocateMem(size_t size, int alignment) {
		int overhang = m_currentPageOffset % alignment;
		int wastage = 0;
		if (overhang) {
			wastage = alignment - overhang;
		}

		int effectiveSize = (int)size + wastage;

		if (!m_currentPage || m_currentPageRemaining < effectiveSize) {
			return AllocateSlowPath(size);
		}

		void *result = (void *)((intptr_t)m_currentPage + (intptr_t)m_currentPageOffset + (intptr_t)wastage);
		m_currentPageOffset += effectiveSize;
		m_currentPageRemaining -= effectiveSize;
		// printf("alloc at %p - %d  %d %d\n", result, effectiveSize, m_currentPageOffset, m_currentPageRemaining);
		// printf("   %p\n", m_currentPage);
		return result;
	}

  private:
	// Alignment is NOT handled in here.
	void *AllocateSlowPath(int size);

	struct MappedBlock {
		void *addr;
		int size;
	};

	std::vector<MappedBlock> m_blocks;

	void *m_currentPage = nullptr;
	int m_currentPageOffset = 0;
	int m_currentPageRemaining = 0;
	int m_pageSize = 0;
};
