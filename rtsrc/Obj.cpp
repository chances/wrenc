//
// Created by znix on 10/07/2022.
//

#include "Obj.h"
#include "Errors.h"
#include "GCTracingScanner.h"
#include "ObjClass.h"
#include "ObjNum.h"
#include "ObjRange.h"
#include "ObjString.h"

#include <cstddef>
#include <string>

// The position of the GC word is part of the ABI
static_assert(offsetof(Obj, gcWord) == GC_WORD_OFFSET, "The GC-word is not at the ABI-defined location!");

Obj::~Obj() = default;
Obj::Obj(ObjClass *type) : gcWord(GCTracingScanner::INITIAL_GC_WORD), type(type) {}

std::string Obj::ConvertToString() {
	// If this is an object of a string, just read it out directly
	ObjString *thisStr = dynamic_cast<ObjString *>(this);
	if (thisStr)
		return thisStr->m_value;

	const static SignatureId SIG = ObjClass::FindSignatureId("toString");
	FunctionTable::Entry *entry = type->LookupMethod(SIG);
	if (!entry) {
		errors::wrenAbort("Object '%s' is missing mandatory toString method!", type->name.c_str());
	}

	typedef Value (*toStringFunc_t)(Value);
	toStringFunc_t toStringFunc = (toStringFunc_t)entry->func;

	Value stringValue = toStringFunc(encode_object(this));

	if (!is_object(stringValue)) {
		errors::wrenAbort("Object '%s' toString returned non-object value", type->name.c_str());
	}

	Obj *obj = (Obj *)get_object_value(stringValue);
	ObjString *str = dynamic_cast<ObjString *>(obj);
	if (!str) {
		errors::wrenAbort("Object '%s' toString returned non-string object type '%s'", type->name.c_str(),
		    obj->type->name.c_str());
	}

	return str->m_value;
}

std::string Obj::ToString(Value value) {
	if (value == NULL_VAL)
		return "null";
	if (is_object(value))
		return get_object_value(value)->ConvertToString();
	return ObjNumClass::Instance()->ToString(get_number_value(value));
}

bool Obj::Same(Value a, Value b) {
	if (is_value_float(a)) {
		return a == b;
	}

	Obj *objA = get_object_value(a);
	Obj *objB = get_object_value(b);

	if (objA == nullptr || objB == nullptr)
		return objA == objB;

	if (objA->type != objB->type)
		return false;

	if (ObjString *str = dynamic_cast<ObjString *>(objA)) {
		return str->EqualTo(objB);
	}
	if (ObjRange *range = dynamic_cast<ObjRange *>(objA)) {
		return range->EqualTo(objB);
	}

	// Booleans are singletons, so comparing them by identity like
	// everything else is fine.
	return objA == objB;
}

// By default, compare by identity
bool Obj::EqualTo(Obj *other) { return other == this; }
bool Obj::OperatorEqualTo(Value other) {
	if (is_value_float(other) || other == NULL_VAL)
		return false;
	return EqualTo(get_object_value(other));
}
bool Obj::OperatorNotEqual(Value other) { return !OperatorEqualTo(other); }

bool Obj::OperatorBoolNegate() {
	// Same as in Wren, see in wren_core.c: object_not
	return false;
}

bool Obj::Is(ObjClass *cls) { return type->Extends(cls); }

ObjClass *Obj::Type() { return type; }

std::string Obj::ToString() { return "instance of " + type->name; }
