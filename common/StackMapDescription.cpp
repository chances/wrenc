//
// Created by znix on 24/12/22.
//

#include "StackMapDescription.h"

#include <algorithm>
#include <optional>
#include <stdio.h>

struct StackMapHeader {
	uint16_t major;  // Major version (breaking changes)
	uint16_t minor;  // Minor version (compatible changes)
	uint16_t flags;  // Flags
	uint16_t unused; // Unused
};

struct FunctionInsn {
	void *functionPtr;
	int numStatepoints;
	int stackSize;
};

struct StatepointInsn {
	uint32_t instructionOffset;
	uint8_t offsets[];
};

StackMapDescription::StackMapDescription(const void *serialised) {
	const StackMapHeader *header = (const StackMapHeader *)serialised;
	serialised = header + 1;

	if (header->major != 1) {
		fprintf(stderr, "Unsupported-by-runtime stackmap major version %d\n", header->major);
		abort();
	}

	// The most recently found function. Statepoints give their instruction pointer relative to the
	// start of the function, so we need this to recover them.
	const FunctionDescriptor *currentFunction = nullptr;

	while (true) {
		const MapObjectRepr *object = (const MapObjectRepr *)serialised;
		const void *afterObj = object + 1;

		switch (object->id) {
		case ObjectID::END_OF_STACK_MAP: {
			goto loop_done;
		}
		case ObjectID::FUNCTION: {
			// All we really need is to remember the function for the statepoints
			const FunctionInsn *insn = (const FunctionInsn *)afterObj;
			int newId = m_functions.size();
			m_functions.emplace_back(FunctionDescriptor{
			    .functionPointer = insn->functionPtr,
			    .id = newId,
			});
			currentFunction = &m_functions.back();

			break;
		}
		case ObjectID::STATEPOINT: {
			const StatepointInsn *insn = (const StatepointInsn *)afterObj;

			int heapStart = m_offsetHeap.size();
			int numOffsets = object->forObject;

			m_offsetHeap.insert(m_offsetHeap.end(), insn->offsets, insn->offsets + numOffsets);

			m_statepoints.emplace_back(InternalStatepointDescriptor{
			    .functionId = currentFunction->id,
			    .instructionOffset = insn->instructionOffset,
			    .offsetIndex = heapStart,
			    .numLocals = numOffsets,
			});
			break;
		}
		default:
			fprintf(stderr, "Found invalid stackmap object: ID=%d\n", (int)object->id);
			abort();
			break;
		}

		// Advance to the next object - the size doesn't include the header
		serialised = (const void *)((uint64_t)object + sizeof(MapObjectRepr) + object->sectionSize);
	}
loop_done:

	// Sort the statepoints by pointer, so we can binary search it later
	struct {
		bool operator()(const InternalStatepointDescriptor &a, const InternalStatepointDescriptor &b) const {
			return outerThis->FindInstructionPtr(a) < outerThis->FindInstructionPtr(b);
		}
		StackMapDescription *outerThis;
	} sortByPtr;
	sortByPtr.outerThis = this;
	std::sort(m_statepoints.begin(), m_statepoints.end(), sortByPtr);
}

std::optional<StackMapDescription::StatepointDescriptor> StackMapDescription::Lookup(void *address) {
	// Binary-search for a matching statepoint
	decltype(m_statepoints)::iterator match = std::lower_bound(m_statepoints.begin(), m_statepoints.end(), address,
	    [this](const InternalStatepointDescriptor &statepoint, void *address) {
		    return FindInstructionPtr(statepoint) < address;
	    });

	// Didn't find anything?
	if (match == m_statepoints.end())
		return std::nullopt;

	// lower_bound will return the first statepoint whose pointer is >= the one we passed, so if the pointers don't
	// match then our statepoint isn't here.
	void *matchInsn = FindInstructionPtr(*match);
	if (matchInsn != address)
		return std::nullopt;

	return StatepointDescriptor{
	    .instruction = matchInsn,
	    .offsets = m_offsetHeap.data() + match->offsetIndex,
	    .numOffsets = match->numLocals,
	};
}

void *StackMapDescription::FindInstructionPtr(const StackMapDescription::InternalStatepointDescriptor &statepoint) {
	const FunctionDescriptor &function = m_functions.at(statepoint.functionId);
	return (void *)((uint64_t)function.functionPointer + statepoint.instructionOffset);
}
