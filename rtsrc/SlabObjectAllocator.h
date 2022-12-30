//
// Created by znix on 30/12/22.
//

#pragma once

#include "LinkedList.h"
#include "Obj.h"

#include <map>
#include <memory>

class ObjManaged;
class ObjManagedClass;

/// A specialised allocator that stores Obj instances.
/// See the comment at the start of the implementation file for implementation notes.
class SlabObjectAllocator {
  public:
	SlabObjectAllocator();
	~SlabObjectAllocator();

	SlabObjectAllocator(const SlabObjectAllocator &) = delete;
	SlabObjectAllocator &operator=(const SlabObjectAllocator &) = delete;

	/// Get the instance from WrenRuntime, without requiring that to be imported.
	static SlabObjectAllocator *GetInstance();

	ObjManaged *AllocateManaged(ObjManagedClass *cls);
	template <typename T, typename... Args> T *AllocateNative(Args &&...args) {
		void *mem = AllocateRaw(T::Class(), sizeof(T));
		T *obj = new (mem) T(std::forward<Args>(args)...);

		// Check that T extends Obj
		Obj *baseObj = obj;
		(void)baseObj;

		return obj;
	}

  private:
	struct Slab;
	struct FreeShim;

	// Linked-list accessors
	struct LLSlabAllAccess;
	struct LLSlabFreeAccess;
	struct LLFreeShimAccess;

	struct SizeCategory {
		int size = -1;

		int numTotal = 0;
		LinkedList<Slab, LLSlabAllAccess> allSlabs;

		int numFree = 0;
		LinkedList<Slab, LLSlabFreeAccess> freeSlabs;

		/// The total number of objects stored across all slabs. Equal to the sum of each slab's object count.
		int totalObjects = 0;

		/// Get the number of bytes where we can actually allocate objects in a slab of this size, when
		/// accounting for the slab struct at the end and any wasted space immediately before that.
		int GetUsableSize() const;
	};

	struct Slab {
		/// A pointer back to the allocator
		SlabObjectAllocator *allocator = nullptr;

		/// The size category this slab resides in
		SizeCategory *sizeCategory = nullptr;

		/// Linked list of all the slabs
		Slab *allNext = nullptr, *allPrev = nullptr;

		/// Linked list of the free slabs
		Slab *freeSlabsNext = nullptr, *freeSlabsPrev = nullptr;

		/// A linked list of all the free-space shims inside the slab's storage memory.
		LinkedList<FreeShim, LLFreeShimAccess> freeSpaceList;

		/// The number of objects currently stored inside this slab
		int currentObjects = 0;

		/// Get the address of the start of the memory region this slab represents
		void *GetSectionBaseAddress();

		/// If replacement==nullptr, then remove this FreeShim from the free space list.
		/// If replacement!=nullptr, then replace the shim with the specified one. This
		///   is required for allocating memory at the start of a shim for example, as
		///   the shim gets moved forwards by one object.
		void SwapOrDeleteShim(FreeShim *shim, FreeShim *replacement);

		/// Remove this slab from the category's list of slabs with free space.
		void RemoveFromFreeSlabList();
	};

	struct FreeShim {
		static constexpr inline uint32_t MAGIC_VALUE = 0xa8acdba2;

		/// The total length of the memory occupied by the shim.
		int length = 0;

		/// This is filled with the contents of MAGIC_VALUE, again as a sanity check.
		/// This position was chosen since it's stored in the same place as an Obj's upper
		/// 32-bits of it's vtable pointer, and our magic value will make quite sure it's
		/// not a valid pointer.
		uint32_t magic = 0;

		FreeShim *next = nullptr, *prev = nullptr;
	};
	static_assert(sizeof(FreeShim) <= sizeof(Obj), "FreeShim might not fit inside an object's space!");

	// Get a pointer to the 'best' size category to contain an item of the given size. This may
	// create a new category.
	SizeCategory *GetOrCreateBestCategory(int size);

	void *AllocateRaw(ObjClass *cls, int size);

	Slab *CreateSlab(SizeCategory *size);

	// Use a sorted map, so we can use lower_bound.
	// Note we have to use unique_ptr as a pointer to the size categories is embedded in the slabs.
	std::map<int, std::unique_ptr<SizeCategory>> m_sizes;

	/// The address to allocate the next slab at.
	void *nextSlabAddr = nullptr;
};
