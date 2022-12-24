//
// Created by znix on 24/12/22.
//

#pragma once

#include <stdint.h>

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
};
