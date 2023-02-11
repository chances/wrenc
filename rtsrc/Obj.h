//
// Created by znix on 10/07/2022.
//

#pragma once

#include "common.h"

#include <string>

// ObjClass is really special
class ObjClass;

// These other classes aren't so special, they're just declared here for convenience.
class ObjMap;
class ObjString;
class ObjRange;

// A macro read by the bindings generator to mark a method as being accessible from Wren.
#define WREN_METHOD(...)

// Used by the binding generator to set the name of an argument
#define ARG(...)

/// A collection of functions provided by the GC to objects which are reporting their contents.
struct GCMarkOps {
	void (*ReportValue)(GCMarkOps *thisObj, Value value);
	void (*ReportValues)(GCMarkOps *thisObj, const Value *values, int count);

	// Reporting null pointers is allowed.
	void (*ReportObject)(GCMarkOps *thisObj, Obj *object);
	void (*ReportObjects)(GCMarkOps *thisObj, Obj *const *objects, int count);

	/// Get the GC implementation. This is provided for the use of ObjFibre,
	/// which has to make the GC unwind the stack of suspended fibres.
	void *(*GetGCImpl)(GCMarkOps *thisObj);
};

/// An object refers to basically anything accessible by Wren (maybe except for future inline classes).
class Obj {
  public:
	virtual ~Obj();
	Obj(ObjClass *type);

	Obj(const Obj &) = delete;
	Obj &operator=(const Obj &) = delete;

	/// The GC word is a 64-bit value that the GC system can use however it wants. For tracing
	/// GCs this would hold state data (eg for a tricolour collector, it'd be a white/grey/black
	/// value), and for reference-counting GCs this is a reference count.
	/// This is initialised in the constructor, from a value set by the GC.
	uint64_t gcWord;

	/// This class's metaclass. This is represented by double arrows in the diagram in
	/// ObjClass's file header.
	/// For metaclasses, this always points to the root 'Class' class.
	/// For non-ObjClass objects, this points to the object's ObjClass.
	ObjClass *type = nullptr;

	inline Value ToValue() { return encode_object(this); }

	/// Called when the GC runs, and the object must report all object pointers or values it
	/// contains. Not doing so could easily lead to freeing reachable objects.
	/// The exception is that classes do not not need (and indeed, should not) mark their
	/// type ObjClass instance - that's handled by the GC.
	virtual void MarkGCValues(GCMarkOps *ops) = 0;

	/// Tests whether this object equals another. Used by the default == and != implementations.
	virtual bool EqualTo(Obj *other);

	// By default, compare identity
	WREN_METHOD() bool OperatorEqualTo(Value other);
	WREN_METHOD() bool OperatorNotEqual(Value other);
	WREN_METHOD(getter) bool OperatorBoolNegate();
	WREN_METHOD() bool Is(ObjClass *cls);
	WREN_METHOD(getter) ObjClass *Type();
	WREN_METHOD(getter) std::string ToString();

	/// Helper method to make a virtual call to the toString function
	/// Note this is called ConvertToString and not ToString to avoid clashing with implementations of the Wren method
	std::string ConvertToString();
	static std::string ToString(Value value);
};
