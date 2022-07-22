//
// Created by znix on 10/07/2022.
//

#pragma once

#include "common.h"

#include <string>

// ObjClass is really special
class ObjClass;

// These other classes aren't so special, they're just declared here for convenience.
class ObjSystem;
class ObjMap;
class ObjString;

/// An object refers to basically anything accessible by Wren (maybe except for future inline classes).
class Obj {
  public:
	virtual ~Obj();
	Obj(ObjClass *type);

	Obj(const Obj &) = delete;
	Obj &operator=(const Obj &) = delete;

	/// This class's metaclass. This is represented by double arrows in the diagram in
	/// ObjClass's file header.
	/// For metaclasses, this always points to the root 'Class' class.
	/// For non-ObjClass objects, this points to the object's ObjClass.
	ObjClass *type = nullptr;

	// TODO reference counting

	inline Value ToValue() { return encode_object(this); }

	/// Helper method to make a virtual call to the toString function
	std::string ToString();
	static std::string ToString(Value value);
};
