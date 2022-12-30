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

#include <assert.h>
#include <inttypes.h>
#include <random>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

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
		globalSlabSize += getpagesize();
	}
}
SlabObjectAllocator::~SlabObjectAllocator() {}

SlabObjectAllocator *SlabObjectAllocator::GetInstance() { return WrenRuntime::Instance().GetObjectAllocator(); }

ObjManaged *SlabObjectAllocator::AllocateManaged(ObjManagedClass *cls) {
	static_assert(alignof(ObjManaged) == 8);
	void *mem = AllocateRaw(cls, cls->size);
	return new (mem) ObjManaged(cls); // Initialise with placement-new
}

SlabObjectAllocator::SizeCategory *SlabObjectAllocator::GetOrCreateBestCategory(int size) {
	// Check we're allocating something large enough, to avoid trying to deal with stuff
	// getting rounded down to zero later.
	if (size < (int)sizeof(Obj)) {
		errors::wrenAbort("Invalid allocation request size: %d\n", size);
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

void *SlabObjectAllocator::AllocateRaw(ObjClass *cls, int requestedSize) {
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
		errors::wrenAbort("Memory allocator: slab free shim misaligned with offset %" PRIx64 "\n", offset);
	}

	int newLength = freeShim->length - category->size;
	if (newLength == 0) {
		// Delete this shim
		slab->SwapOrDeleteShim(freeShim, nullptr);
	} else {
		if (newLength < 0 || (newLength % category->size) != 0) {
			errors::wrenAbort("Memory allocator: slab free shim length misaligned: %d\n", newLength);
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

	// The FreeShim was a placeholder, occupying the memory where an object
	// will be allocated until it's used for an actual Object.
	void *mem = (void *)freeShim;

	// Zero the memory as a matter of good practice
	memset(mem, 0, category->size);

	return mem;
}

SlabObjectAllocator::Slab *SlabObjectAllocator::CreateSlab(SlabObjectAllocator::SizeCategory *size) {
	// TODO Windows port

	// Linux doesn't have a good way to ask for a large alignment with mmap, so pick
	// a random address and on a 64-bit system it's extremely unlikely for it to be occupied.
	// Once we've picked one address, allocate the others adjacent to it (to hopefully
	// reduce overheads in the kernel's VM system) unless we run into something.
	if (nextSlabAddr == nullptr) {
		std::random_device rng;
		uint64_t random64 = ((uint64_t)rng() << 32) | rng();

		// Throw away the top 17 bits, as they're only one for kernel addresses
		random64 >>= 17;

		// Align the address to our desired slab size, so the start of the memory is always aligned
		random64 &= ~(uint64_t)(globalSlabSize - 1);

		nextSlabAddr = (void *)random64;
	}

	void *mem = mmap(nextSlabAddr, globalSlabSize, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (mem == MAP_FAILED) {
		if (errno == EEXIST) {
			// We ran into something! Try a different random address.
			nextSlabAddr = nullptr;
			return CreateSlab(size);
		}

		errors::wrenAbort("Failed to allocate new slab (object size %d, slab size %d)\n", size->size, globalSlabSize);
	}
	assert(mem == nextSlabAddr);

	nextSlabAddr = (void *)((uint64_t)nextSlabAddr + globalSlabSize);

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
