//
// Created by znix on 24/12/22.
//

// Only use 'local unwinding' - see https://www.nongnu.org/libunwind/man/libunwind(3).html
#define UNW_LOCAL_ONLY

#include "GCTracingScanner.h"
#include "Obj.h"
#include "RtModule.h"
#include "StackMapDescription.h"
#include "WrenRuntime.h"

#include <cstdio>
#include <libunwind.h>

using SMD = StackMapDescription;

constexpr bool PRINT_STACK_WALK_DBG = false;

GCTracingScanner::GCTracingScanner() {
	// Build the functions list
	for (const auto &entry : WrenRuntime::Instance().m_userModules) {
		StackMapDescription *stackMap = entry.second->GetStackMap();
		for (const SMD::FunctionDescriptor &function : stackMap->GetFunctions()) {
			m_functions.emplace_back(FunctionInfo{
			    .functionPointer = function.functionPointer,
			    .stackMap = stackMap,
			});
		}
	}

	struct {
		bool operator()(const FunctionInfo &a, const FunctionInfo &b) const {
			return a.functionPointer < b.functionPointer;
		}
	} functionSorter;
	std::sort(m_functions.begin(), m_functions.end(), functionSorter);
}

GCTracingScanner::~GCTracingScanner() {}

void GCTracingScanner::MarkCurrentThreadRoots() {
	unw_cursor_t cursor;
	unw_context_t uc;
	unw_word_t ip, sp;

	unw_getcontext(&uc);
	unw_init_local(&cursor, &uc);
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
			printf("\tstatepoint with %d values\n", statepoint.numOffsets);

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
	if (value == NULL_VAL || is_value_float(value))
		return;

	// TODO implement
	std::string str = Obj::ToString(value);
	printf("TODO IMPLEMENT: Marking object '%s' as GC root\n", str.c_str());
}
