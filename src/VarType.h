//
// Created by znix on 23/02/23.
//

#pragma once

#include <inttypes.h>
#include <string>

class PhiTempType;
class ArenaAllocator;

class VarType {
  public:
	enum Type : int32_t {
		NULL_TYPE,
		NUM,
		OBJECT,        /// Some unknown object, may be an instance of either a C++ or Wren class.
		OBJECT_SYSTEM, /// Some instance of a C++ class, or the class object itself.
	};

	/// Set on OBJECT_SYSTEM types to indicate this variable contains the specified
	/// class object, not an instance of it.
	constexpr static int FLAG_STATIC = 1 << 0;

	// Type and flags pack into 64 bits, put them together to align for pointers later.
	Type type = NULL_TYPE;
	uint32_t flags = 0;

	union {
		/// If this is of type OBJECT_SYSTEM, the name of the C++ object type.
		/// This is the name used by C++, not by Wren - eg ObjString instead of String.
		/// This should only be used for ObjNull or ObjNumClass with FLAG_STATIC.
		const char *systemClassName = nullptr;
	};

	VarType();
	VarType(Type type);

	static VarType *SysClass(ArenaAllocator *allocator, const char *systemClassName);

	std::string ToString() const;
};
