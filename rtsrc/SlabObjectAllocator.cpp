//
// The slab memory allocator - this manages the allocation of memory for managed
// objects, by allocating memory from blocks mapped in from the OS.
//
// The design is as follows, representing memory as a tree:
// * The size buckets - objects of the same or similar (rounding up) size are stored together
//   * Slabs - a slab represents a contiguous set of pages storing a list of items, and
//     contains usage information (how many objects are in use?) and the free list pointers.
//     * Objects - objects are placed into this memory, without special headers or anything.
//     * Free shims - a free shim is a marker placed into an unused part of the slab's
//       memory to indicate there's nothing there. These are variable length, and should
//       never appear adjacent to each other in memory. This allows for efficient iteration
//       over all the objects in a slab (even when almost empty), as you can skip the large,
//       empty portions quickly.
//     * Slab metadata - this is where the data about the slab is stored, and it's placed at
//       the end of every block of n pages inside the slab.
//       Since we know it's size and that it's at the end of the page, we can find it's pointer
//       from the pointer to an object, by rounding down the object's address to the start of
//       the page and adding a fixed offset.
//
// A slab contains a doubly-linked list of all the free shims. When a slab is created, it's
// filled with a single free shim that covers the entire available space
//
// When multiple slabs of the same size have a significant amount of free space available, we
// can merge them together by copying the objects from one to the other, to get an empty slab
// we can then free. The statepoints that the RS4GC LLVM pass generates support relocating
// objects: if we edit the stack where the values are stored, the function won't notice that
// it's objects were moved around in memory. This lets us avoid fragmentation, where the
// application creates a lot of objects then frees most of them, leaving a few live objects
// in each page.
//
// This design is based off Jeff Bonwick's paper. Some modifications have been made
// since we're in a very different environment (we're rarely going to deal with
// objects larger than 100 bytes or so - the contents of lists, maps and strings
// are allocated using a different allocator, and we can relocate objects) and
// hardware has changed a lot in 30 years.
// https://www.usenix.org/legacy/publications/library/proceedings/bos94/bonwick.html
//
// Created by znix on 30/12/22.
//

#include "SlabObjectAllocator.h"
#include "Errors.h"
#include "ObjManaged.h"
#include "WrenRuntime.h"
#include "common/Platform.h"

#include <assert.h>
#include <inttypes.h>
#include <random>
#include <string.h>

namespace mm = mem_management;

// The size of slabs on this system. This is derived from the system page size, so we only want to
// calculate it once. It has to be a global variable since we use it to find the slab pointer, before
// we have a pointer to any of the allocator-related structs.
static int globalSlabSize = 0;

/// A list of sizes that it's always a good idea to have a slab allocator for. Creating a slab in a size
/// category on this list will always be preferred to fallback sizing.
static const int PREFERRED_SIZES[] = {
    // These are all a bit arbitrary, but they seem like common numbers of fields for objects to have.
    // On most platforms std::string and std::vector are 24-bits, so one slot in particular matters
    // to get no padding on those very common objects.
    sizeof(Obj),
    sizeof(Obj) + sizeof(Value) * 1,
    sizeof(Obj) + sizeof(Value) * 2,
    sizeof(Obj) + sizeof(Value) * 3, // Strings and lists
    sizeof(Obj) + sizeof(Value) * 4,
    sizeof(Obj) + sizeof(Value) * 5,
    sizeof(Obj) + sizeof(Value) * 6,
    sizeof(Obj) + sizeof(Value) * 8,
    sizeof(Obj) + sizeof(Value) * 10,
    sizeof(Obj) + sizeof(Value) * 12,
    sizeof(Obj) + sizeof(Value) * 16,
};
static_assert(sizeof(Obj) == 24);
static_assert(sizeof(Value) == 8);

struct SlabObjectAllocator::LLSlabAllAccess {
	static Slab **Next(Slab *slab) { return &slab->allNext; }
	static Slab **Prev(Slab *slab) { return &slab->allPrev; }
};
struct SlabObjectAllocator::LLSlabFreeAccess {
	static Slab **Next(Slab *slab) { return &slab->freeSlabsNext; }
	static Slab **Prev(Slab *slab) { return &slab->freeSlabsPrev; }
};
struct SlabObjectAllocator::LLFreeShimAccess {
	static FreeShim **Next(FreeShim *slab) { return &slab->next; }
	static FreeShim **Prev(FreeShim *slab) { return &slab->prev; }
};

static int roundToAlignment(int value) {
	int alignment = 8;
	int overhang = value % alignment;
	if (overhang)
		value += alignment - overhang;
	return value;
}

SlabObjectAllocator::SlabObjectAllocator() {
	// Use 16k slabs, or as near as we can - it's a pretty arbitrary amount, and it doesn't matter that much.
	while (globalSlabSize < 16 * 1024) {
		globalSlabSize += mm::getPageSize();
	}
}
SlabObjectAllocator::~SlabObjectAllocator() {}

SlabObjectAllocator *SlabObjectAllocator::GetInstance() { return WrenRuntime::Instance().GetObjectAllocator(); }

ObjManaged *SlabObjectAllocator::AllocateManaged(ObjManagedClass *cls) {
	static_assert(alignof(ObjManaged) == 8);
	void *mem = AllocateRaw(cls->size);
	return new (mem) ObjManaged(cls); // Initialise with placement-new
}

SlabObjectAllocator::SizeCategory *SlabObjectAllocator::GetOrCreateBestCategory(int size) {
	// Check we're allocating something large enough, to avoid trying to deal with stuff
	// getting rounded down to zero later.
	if (size < (int)sizeof(Obj)) {
		errors::wrenAbort("Invalid allocation request size: %d", size);
	}

	// Find the smallest size category that fits the object
	decltype(m_sizes)::iterator bestExisting = m_sizes.lower_bound(size);

	// If we've found a category of the exact size, then obviously use that
	if (bestExisting != m_sizes.end() && bestExisting->first == size) {
		return bestExisting->second.get();
	}

	// Use power-of-sqrt(2) sizing to pick size categories while minimising wasted padding
	// Note that these allocations are only for objects of this size, so we can be a lot
	// more generous about creating new size categories than a general purpose allocator
	// which has to deal with allocating memory for things like dynamically generated
	// arrays, where it might only ever see one allocation for a given size category.
	//
	// It might seem odd to use sqrt(2), but it means every second entry is a power-of-two
	// size, so it's easy to calculate. Only do it roughly, and align it to the 8-byte
	// boundary since we're going to be storing pointers anyway.

	// First, round up to the next power of two
	int powerOfTwo = std::bit_ceil((unsigned)size);

	// Divide by sqrt(2) to find the 'in-between' value, the value in the power-of-sqrt(2)
	// series that isn't present in the power-of-2 series.
	int inBetween = (int)((float)powerOfTwo / 1.414);
	inBetween = roundToAlignment(inBetween);

	// Select the smallest size that will fit
	int bestSize = inBetween >= size ? inBetween : powerOfTwo;

	// Check if there's a preferred size that's lower than this - if so, then use that
	// size instead.
	const int *lowerBound = std::lower_bound(PREFERRED_SIZES, std::cend(PREFERRED_SIZES), size);
	if (lowerBound != std::cend(PREFERRED_SIZES) && *lowerBound < bestSize) {
		bestSize = *lowerBound;
	}

	// Make sure we've picked a large enough size
	assert(bestSize >= size);

	// Is there already such a category?
	bestExisting = m_sizes.find(bestSize);
	if (bestExisting != m_sizes.end())
		return bestExisting->second.get();

	// Create a category
	std::unique_ptr<SizeCategory> category = std::make_unique<SizeCategory>();
	SizeCategory *ptr = category.get();
	ptr->size = bestSize;
	m_sizes[ptr->size] = std::move(category);

	return ptr;
}

void *SlabObjectAllocator::AllocateRaw(int requestedSize) {
	SizeCategory *category = GetOrCreateBestCategory(requestedSize);
	assert(category->size >= requestedSize && "found category too small for requested allocation");

	// Get or create a slab with free space
	Slab *slab = category->freeSlabs.start;
	if (!slab)
		slab = CreateSlab(category);
	assert(slab->freeSlabsPrev == nullptr && "Slab was at the start of the free list but had a previous ptr");

	FreeShim *freeShim = slab->freeSpaceList.start;
	assert(freeShim && "Slab was on the free list, but didn't have a free space shim!");
	assert(freeShim->prev == nullptr && "FreeShim at the start of the free space list had a previous ptr");

	// Check the shim is properly aligned
	int64_t offset = (int64_t)freeShim - (int64_t)slab->GetSectionBaseAddress();
	if (offset < 0 || (offset % category->size) != 0) {
		errors::wrenAbort("Memory allocator: slab free shim misaligned with offset %" PRIx64, offset);
	}

	int newLength = freeShim->length - category->size;
	if (newLength == 0) {
		// Delete this shim
		slab->SwapOrDeleteShim(freeShim, nullptr);
	} else {
		if (newLength < 0 || (newLength % category->size) != 0) {
			errors::wrenAbort("Memory allocator: slab free shim length misaligned: %d", newLength);
		}

		// Move the free slab along a bit.
		FreeShim *newShim = (FreeShim *)((int64_t)freeShim + category->size);
		*newShim = {
		    .length = newLength,
		    .magic = FreeShim::MAGIC_VALUE,
		};

		// This sets up the next and prev pointers
		slab->SwapOrDeleteShim(freeShim, newShim);
	}

	slab->currentObjects++;

	// The FreeShim was a placeholder, occupying the memory where an object
	// will be allocated until it's used for an actual Object.
	void *mem = (void *)freeShim;

	// Zero the memory as a matter of good practice
	memset(mem, 0, category->size);

	return mem;
}

SlabObjectAllocator::Slab *SlabObjectAllocator::CreateSlab(SlabObjectAllocator::SizeCategory *size) {
	// Linux doesn't have a good way to ask for a large alignment with mmap, so pick
	// a random address and on a 64-bit system it's extremely unlikely for it to be occupied.
	// Once we've picked one address, allocate the others adjacent to it (to hopefully
	// reduce overheads in the kernel's VM system) unless we run into something.
	if (m_nextSlabAddr == nullptr) {
		std::random_device rng;
		uint64_t random64 = ((uint64_t)rng() << 32) | rng();

		// Throw away the top 17 bits, as they're only one for kernel addresses
		random64 >>= 17;

		// Align the address to our desired slab size, so the start of the memory is always aligned
		random64 &= ~(uint64_t)(globalSlabSize - 1);

		m_nextSlabAddr = (void *)random64;
	}

	bool collided;
	bool success = mm::allocateMemoryAtAddress(m_nextSlabAddr, globalSlabSize, collided);
	if (!success) {
		if (collided) {
			// We ran into something! Try a different random address.
			m_nextSlabAddr = nullptr;
			return CreateSlab(size);
		}

		// Don't use a normal wren abort, that would involve allocating memory for a string
		fprintf(stderr, "Wren Runtime: Failed to allocate new slab (address %p, object size %d, slab size %d)\n",
		    m_nextSlabAddr, size->size, globalSlabSize);
		abort();
	}

	void *mem = m_nextSlabAddr;
	m_nextSlabAddr = (void *)((uint64_t)m_nextSlabAddr + globalSlabSize);

	Slab *slab = (Slab *)((uint64_t)mem + globalSlabSize - sizeof(Slab));
	*slab = {.sizeCategory = size};

	// Append this slab to the end of the free and total lists - the total list tracks
	// all the slabs (full or not) in this size class, and this slab obviously has
	// free space in it since we just allocated it.
	size->allSlabs.InsertAtEnd(slab);
	size->freeSlabs.InsertAtEnd(slab);

	// Create a single huge free space shim to cover the entire usable contents of the slab
	FreeShim *shim = (FreeShim *)mem;
	*shim = {
	    .length = size->GetUsableSize(),
	    .magic = FreeShim::MAGIC_VALUE,
	};
	slab->freeSpaceList.InsertAtStart(shim);

	return slab;
}

void SlabObjectAllocator::DeallocateUnreachableObjects(uint64_t reachableGCWord) {
	// Run the free sweep on each slab individually
	for (const auto &entry : m_sizes) {
		SizeCategory *size = entry.second.get();
		for (Slab *slab = size->allSlabs.start; slab != nullptr; slab = slab->allNext) {
			DeallocateUnreachableObjectsForSlab(slab, reachableGCWord);
		}
	}
}

void SlabObjectAllocator::DeallocateUnreachableObjectsForSlab(Slab *slab, uint64_t reachableGCWord) {
	// Walk through every object, and collapse them into free blocks if they don't have the GC word
	// indicating they're reachable.
	void *ptr = slab->GetSectionBaseAddress();
	void *endPtr = (void *)((uint64_t)ptr + slab->sizeCategory->GetUsableSize());

	// If the last object was a free shim, this is set so we can merge stuff into it
	FreeShim *lastFreeShim = nullptr;

	// Re-count the number of objects in the slab - this is to produce a new
	// total, so exclude those we delete.
	int objectCount = 0;

	while (ptr != endPtr) {
		assert(ptr < endPtr && "overran slab area during GC free pass");

		// We don't know if there's an object or free shim here, so check the magic
		FreeShim *freeShim = (FreeShim *)ptr;
		Obj *obj = (Obj *)ptr;

		// The magic can never appear in a real vtable pointer, since it covers the
		// upper 32 bits of the vtable pointer and it's upper 16 bits aren't the
		// same, making it impossible to use on amd64 and aarch64.
		if (freeShim->magic == FreeShim::MAGIC_VALUE) {
			if (lastFreeShim != nullptr) {
				// If there's a free shim behind us too, then merge the two together
				lastFreeShim->length += freeShim->length;
				slab->freeSpaceList.Remove(freeShim);
			} else {
				// Otherwise, mark this as being the last free shim, and the next
				// object might merge into it.
				lastFreeShim = freeShim;
			}

			// Advance over this shim's size, to get to the next object.
			// Note that if we just merged this shim into the last one, this is still
			// the right thing to do.
			ptr = (void *)((uint64_t)ptr + freeShim->length);
			continue;
		}

		// We've got a single object here, so advance the pointer over it now, since this
		// is the same for both reachable and unreachable objects.
		ptr = (void *)((uint64_t)ptr + slab->sizeCategory->size);

		// Check if this object is reachable. If so, then jump over it
		if (obj->gcWord == reachableGCWord) {
			// Since this object will be the previous one next loop, make sure that
			// no-one tries to expand the free shim behind us.
			lastFreeShim = nullptr;
			objectCount++;
			continue;
		}

		// Run this object's destructor to free any resources (eg std::vector) it
		// might be holding onto.
		obj->~Obj();

		// Null out the object's vtable pointer, so the user will fault if they try
		// accessing it. Otherwise the object could go untouched if we expanded in a
		// previous slab, which could be very annoying to debug.
		*(void **)obj = nullptr;

		// The object is unreachable, free up it's space.
		// If there's a free shim behind us then expand that, otherwise make a new one.
		if (lastFreeShim) {
			lastFreeShim->length += slab->sizeCategory->size;
			// Leave lastFreeShim set, since it's still the previous shim
			continue;
		}

		// Create a new free shim in this object's space.
		*freeShim = FreeShim{
		    .length = slab->sizeCategory->size,
		    .magic = FreeShim::MAGIC_VALUE,
		};
		slab->freeSpaceList.InsertAtEnd(freeShim);

		// Mark this shim as being the last known one, so it'll get expanded
		// into subsequent free objects.
		lastFreeShim = freeShim;
	}

	slab->currentObjects = objectCount;

	// Figure out if this slab was previously on the free slabs list
	bool wasMarkedFree = slab->sizeCategory->freeSlabs.start == slab || slab->freeSlabsPrev != nullptr;

	// If it wasn't but it now has free space, then add it to that list again
	if (!wasMarkedFree && slab->freeSpaceList.start != nullptr) {
		slab->sizeCategory->freeSlabs.InsertAtEnd(slab);
	}
}

void *SlabObjectAllocator::Slab::GetSectionBaseAddress() {
	// Start at our pointer
	// Add our size, ending up at the end of the slab
	// Subtract the slab size to get back to it's start
	return (void *)(((uint64_t)this) + sizeof(*this) - globalSlabSize);
}

void SlabObjectAllocator::Slab::SwapOrDeleteShim(FreeShim *shim, FreeShim *replacement) {
	if (replacement) {
		freeSpaceList.InsertAfter(shim, replacement);
	}
	freeSpaceList.Remove(shim);

	// If the free space list is now empty, then remove ourselves from the free slabs list
	if (freeSpaceList.start == nullptr) {
		RemoveFromFreeSlabList();
	}
}

void SlabObjectAllocator::Slab::RemoveFromFreeSlabList() {
	assert(!freeSpaceList.start && "Cannot mark a slab as having no more free space if that's untrue");

	// And if we're all out of free space, remove this slab from the size category's free slab list
	if (freeSlabsNext)
		freeSlabsNext->freeSlabsPrev = freeSlabsPrev;
	if (freeSlabsPrev)
		freeSlabsPrev->freeSlabsNext = freeSlabsNext;

	sizeCategory->freeSlabs.Remove(this);
}

int SlabObjectAllocator::SizeCategory::GetUsableSize() const {
	int totalAvailable = globalSlabSize - sizeof(Slab);
	int wasted = totalAvailable % size;
	return totalAvailable - wasted;
}

int SlabObjectAllocator::SizeCategory::GetNumSlots() const {
	int totalAvailable = globalSlabSize - sizeof(Slab);
	return totalAvailable / size; // rounds down
}
