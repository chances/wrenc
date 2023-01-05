//
// Created by znix on 24/12/22.
//

// Only use 'local unwinding' - see https://www.nongnu.org/libunwind/man/libunwind(3).html
#define UNW_LOCAL_ONLY

#include "GCTracingScanner.h"
#include "Obj.h"
#include "ObjFibre.h"
#include "RtModule.h"
#include "SlabObjectAllocator.h"
#include "StackMapDescription.h"
#include "WrenRuntime.h"

#include <cstdio>
#include <libunwind.h>
#include <stdint.h>

using SMD = StackMapDescription;

constexpr bool PRINT_STACK_WALK_DBG = false;

// This is a simple tri-colour GC. Each object can be marked as black (unvisited, might be unreachable), grey (reachable
// but we haven't yet examined it) and white (reachable and we have examined it). All reachable objects are first
// marked as grey, and later on when the object is analysed and objects reachable via it are marked as grey, it
// changes to white to indicate further analysis isn't required.
//
// We have a single 64-bit value reserved in the object for our use. We'll use this to store both the current colour
// of the object, and as a linked list of all the grey objects. We'll have a number that the GC considers white,
// and any object marked with that number is white. Any object marked with a different number is black (they might
// have been white in a previous GC cycle, but the global white-ness number will change with each cycle to avoid
// having to edit this word for every object between cycles. Anything with a pointer will be grey.
// We'll encode numbers and pointers using a custom system (we previously used the standard NaN-tagging system, but
// that caused some performance problems on the extremely hot AddToGreyList function, that we can solve by not
// supporting floating-point numbers). Any even number is a pointer, and any odd number is not. Alignment ensures
// that we won't ever have odd object addresses.

static bool isGrey(uint64_t gcWord) {
	// Pointers are aligned to 8 bytes, so they're always even.
	// Black and white values are always kept odd, so this works.
	// It's a single-byte and, so hopefully it should be very efficient since we'll do this a LOT.
	return (gcWord & 1) == 0;
}

static uint64_t pointerToGrey(Obj *ptr) {
	// Pointers are stored as GC words without modification
	return (uint64_t)ptr;
}

static Obj *greyToPointer(uint64_t gcWord) {
	// Again, pointers are stored without any changes
	return (Obj *)gcWord;
}

GCTracingScanner::GCTracingScanner() {
	// Build the functions list
	auto addFunction = [this](StackMapDescription *stackMap) {
		for (const SMD::FunctionDescriptor &function : stackMap->GetFunctions()) {
			m_functions.emplace_back(FunctionInfo{
			    .functionPointer = function.functionPointer,
			    .stackMap = stackMap,
			});
		}
	};
	addFunction(WrenRuntime::Instance().m_coreModule->GetStackMap());
	for (const auto &entry : WrenRuntime::Instance().m_userModules) {
		StackMapDescription *stackMap = entry.second->GetStackMap();
		addFunction(stackMap);
	}

	struct {
		bool operator()(const FunctionInfo &a, const FunctionInfo &b) const {
			return a.functionPointer < b.functionPointer;
		}
	} functionSorter;
	std::sort(m_functions.begin(), m_functions.end(), functionSorter);

	// Set up our marking operations - these are the functions that objects call
	// when walking the heap.
	m_markOps.ReportValue = OpsReportValue;
	m_markOps.ReportValues = OpsReportValues;
	m_markOps.ReportObject = OpsReportObject;
	m_markOps.ReportObjects = OpsReportObjects;
	m_markOps.GetGCImpl = OpsGetGCImpl;
	m_markOps.scanner = this;
}

GCTracingScanner::~GCTracingScanner() {}

void GCTracingScanner::BeginGCCycle() {
	// Make sure there wasn't an ongoing GC cycle previously.
	if (m_greyList) {
		fprintf(stderr, "Started a GC cycle with remaining grey objects - this is not allowed!\n");
		abort();
	}

	// This effectively clears out the old root set. Any previously-white values are redefined as black.
	// Increment by two to ensure it remains odd.
	m_currentWhiteNumber += 2;
}

void GCTracingScanner::MarkCurrentThreadRoots() {
	// Mark the current fibre now, since ObjFibre only marks suspended fibres.
	unw_context_t uc;
	unw_getcontext(&uc);
	MarkThreadRoots(&uc);

	// Mark all the threads on the thread callstack as reachable, as they can
	// be resumed by yielding.
	for (ObjFibre *fibre : ObjFibre::fibreCallStack) {
		// This will call MarkThreadRoots.
		fibre->MarkGCValues(&m_markOps);
	}
}

void GCTracingScanner::MarkThreadRoots(void *unwindContext) {
	unw_context_t *uc = (unw_context_t *)unwindContext;

	unw_cursor_t cursor;
	unw_word_t ip, sp;

	unw_init_local(&cursor, uc);
	while (unw_step(&cursor) > 0) {
		unw_get_reg(&cursor, UNW_REG_IP, &ip);
		unw_get_reg(&cursor, UNW_REG_SP, &sp);
		if (PRINT_STACK_WALK_DBG)
			printf("ip = %lx, sp = %lx\n", (long)ip, (long)sp);

		// Try and find a function which might contain this instruction - this is assuming that the contents
		// of a function start at its function pointer and continues until the next function. This is conservative,
		// so we might find a function that doesn't actually contain this pointer. It's fine though, since we'll
		// try looking up the exact address in this function so we'll find out if it's wrong then.
		struct {
			bool operator()(const FunctionInfo &a, unw_word_t b) const { return (uint64_t)a.functionPointer < b; }
		} functionCompare;
		decltype(m_functions)::iterator match =
		    std::lower_bound(m_functions.begin(), m_functions.end(), ip, functionCompare);

		// We've found the first function whose pointer is greater than the instruction pointer - thus (assuming
		// we didn't get the first item, in which case there's no match) we have to decrement the iterator to
		// find the function proceeding our instruction.
		// Note that if lower_bound returns m_functions.end(), then that's still valid - we're going to decrement
		// it, so we do get a valid iterator.
		if (match == m_functions.begin())
			continue;
		match--;

		// Look for this statepoint
		std::optional<SMD::StatepointDescriptor> maybeStatepoint = match->stackMap->Lookup((void *)ip);
		if (!maybeStatepoint)
			continue;
		const SMD::StatepointDescriptor &statepoint = maybeStatepoint.value();

		if (PRINT_STACK_WALK_DBG)
			printf("\tstatepoint '%s' with %d values\n", statepoint.function->name.c_str(), statepoint.numOffsets);

		// We've found a valid statepoint. Now find all the values on it's stack. These are all a positive
		// multiple of eight added to the stack pointer. The offset of the values from the stack pointer have
		// been divided by eight to make them fit into an 8-bit unsigned int.
		for (int i = 0; i < statepoint.numOffsets; i++) {
			uint8_t rawOffset = statepoint.offsets[i];
			int offset = rawOffset * 8;
			Value *valuePtr = (Value *)((intptr_t)sp + offset);
			MarkValueAsRoot(*valuePtr);
		}
	}
}

void GCTracingScanner::MarkValueAsRoot(Value value) {
	// Numbers and null don't need marking
	if (is_value_float(value))
		return;

	AddToGreyList(get_object_value(value));
}

void GCTracingScanner::AddModuleRoots(RtModule *mod) { mod->MarkModuleGCValues(&m_markOps); }

void GCTracingScanner::AddToGreyList(Obj *obj) {
	// ObjMap sets the LSB on pointers when iterating, so it can return null
	// and false without them being considered falsey values and terminating
	// the loop. Thus we have special support here for that: clear the LSB
	// of our pointer, so we can see the original value.
	obj = (Obj *)((uint64_t)obj & (~(uint64_t)1));

	if (!obj)
		return;

	uint64_t gcWord = obj->gcWord;

	// If we've found an already-grey value, then we don't need to do anything.
	if (isGrey(gcWord))
		return;

	// Similarly, white objects have already been scanned.
	if (gcWord == m_currentWhiteNumber)
		return;

	// Mark an object as grey by inserting it into the linked list of grey objects
	obj->gcWord = pointerToGrey(m_greyList);
	m_greyList = obj;
}

void GCTracingScanner::EndGCCycle() {
	// Walk over all the objects, effectively performing a depth-first (since we're using a singly-linked list and
	// are thus inserting and removing at the same end, we can only do depth-first walks - but that's just fine).

	while (m_greyList) {
		Obj *obj = m_greyList;
		uint64_t gcWord = obj->gcWord;
		if (!isGrey(gcWord)) {
			fprintf(stderr,
			    "Found object with non-grey GC word in the grey list. White marker %x, encoded word %" PRIx64 ".",
			    m_currentWhiteNumber, gcWord);
			abort();
		}
		m_greyList = greyToPointer(gcWord);

		obj->MarkGCValues(&m_markOps);

		obj->gcWord = m_currentWhiteNumber;
	}

	// Deallocate all black objects, as we've confirmed they're unreachable.
	SlabObjectAllocator::GetInstance()->DeallocateUnreachableObjects(m_currentWhiteNumber);
}

void GCTracingScanner::OpsReportValue(GCMarkOps *thisObj, Value value) {
	MarkOpsImpl *impl = (MarkOpsImpl *)thisObj;

	if (is_object(value))
		impl->scanner->AddToGreyList(get_object_value(value));
}
void GCTracingScanner::OpsReportValues(GCMarkOps *thisObj, const Value *values, int count) {
	MarkOpsImpl *impl = (MarkOpsImpl *)thisObj;

	for (int i = 0; i < count; i++) {
		Value value = values[i];
		if (is_object(value))
			impl->scanner->AddToGreyList(get_object_value(value));
	}
}
void GCTracingScanner::OpsReportObject(GCMarkOps *thisObj, Obj *object) {
	MarkOpsImpl *impl = (MarkOpsImpl *)thisObj;
	impl->scanner->AddToGreyList(object);
}
void GCTracingScanner::OpsReportObjects(GCMarkOps *thisObj, Obj *const *objects, int count) {
	MarkOpsImpl *impl = (MarkOpsImpl *)thisObj;
	for (int i = 0; i < count; i++) {
		impl->scanner->AddToGreyList(objects[i]);
	}
}

void *GCTracingScanner::OpsGetGCImpl(GCMarkOps *thisObj) {
	MarkOpsImpl *impl = (MarkOpsImpl *)thisObj;
	return impl->scanner;
}
