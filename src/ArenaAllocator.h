//
// Created by znix on 10/07/2022.
//

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <utility>
#include <vector>

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
		int overhang = m_currentPageOffset - (m_currentPageOffset % alignment);
		int wastage = 0;
		if (overhang) {
			wastage = alignment - overhang;
		}

		if (!m_currentPage || m_currentPageRemaining < (int)size + wastage) {
			return AllocateSlowPath(size);
		}

		return (void *)((intptr_t)m_currentPage + wastage);
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
