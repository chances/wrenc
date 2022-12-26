//
// Created by znix on 24/12/22.
//

#pragma once

#include <optional>
#include <stdint.h>
#include <vector>

class StackMapDescription {
  public:
	enum class ObjectID : uint16_t {
		INVALID = 0,
		END_OF_STACK_MAP,
		FUNCTION,
		STATEPOINT,
	};

	/// Serialisation structure - this is the header for each object
	struct MapObjectRepr {
		ObjectID id = ObjectID::INVALID;
		uint16_t sectionSize = 0; // The size of the section, not including the header
		uint16_t flags = 0;
		uint16_t forObject = 0; // The meaning of this depends on the object
	};

	/// The runtime format for a single statepoint
	struct StatepointDescriptor {
		/// The pointer to the instruction that will appear in the instruction pointer when
		/// this statepoint is reached.
		void *instruction = nullptr;

		/// The array of offsets (relative to RSP) used to calculate the pointers to all
		/// the local variables, which are spilled to the stack.
		uint8_t *offsets = nullptr;

		/// The number of offsets - this is the number of local variables we can access
		int numOffsets = 0;
	};

	struct FunctionDescriptor {
		void *functionPointer = nullptr;

		int id = -1;
	};

	StackMapDescription(const void *serialised);

	std::optional<StackMapDescription::StatepointDescriptor> Lookup(void *address);

	inline const std::vector<FunctionDescriptor> &GetFunctions() const { return m_functions; }

  private:
	/// Same as StatepointDescriptor but more compact, and has indexes into the offsets array
	struct InternalStatepointDescriptor {
		/// The index of the function in the m_functions table.
		int functionId = -1;

		/// The position of the instruction which will appear in the stacktrace, relative to the function pointer.
		uint32_t instructionOffset = 0;

		/// The offset in the big offsets array, stating where the start of the offsets list is.
		int offsetIndex = 0;

		/// The number of offsets (indicating local variables) that come after offsetIndex.
		int numLocals = 0;
	};

	void *FindInstructionPtr(const InternalStatepointDescriptor &statepoint);

	std::vector<FunctionDescriptor> m_functions;
	std::vector<InternalStatepointDescriptor> m_statepoints;
	std::vector<uint8_t> m_offsetHeap;
};
