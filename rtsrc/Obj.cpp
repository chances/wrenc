//
// Created by znix on 10/07/2022.
//

#include "Obj.h"
#include "ObjClass.h"
#include "ObjString.h"

#include <string>

Obj::~Obj() = default;
Obj::Obj(ObjClass *type) : type(type) {}

std::string Obj::ToString() {
	// If this is an object of a string, just read it out directly
	ObjString *thisStr = dynamic_cast<ObjString *>(this);
	if (thisStr)
		return thisStr->m_value;

	const static SignatureId SIG = ObjClass::FindSignatureId("toString");
	FunctionTable::Entry *entry = type->LookupMethod(SIG);
	if (!entry) {
		fprintf(stderr, "Object '%s' is missing mandatory toString method!\n", type->name.c_str());
		abort();
	}

	typedef Value (*toStringFunc_t)(Value);
	toStringFunc_t toStringFunc = (toStringFunc_t)entry->func;

	Value stringValue = toStringFunc(encode_object(this));

	if (!is_object(stringValue)) {
		fprintf(stderr, "Object '%s' toString returned non-object value\n", type->name.c_str());
		abort();
	}

	Obj *obj = (Obj *)get_object_value(stringValue);
	ObjString *str = dynamic_cast<ObjString *>(obj);
	if (!str) {
		fprintf(stderr, "Object '%s' toString returned non-string object type '%s'\n", type->name.c_str(),
		        obj->type->name.c_str());
	}

	return str->m_value;
}

std::string Obj::ToString(Value value) {
	if (value == NULL_VAL)
		return "null";
	if (is_object(value))
		return get_object_value(value)->ToString();
	return std::to_string(get_number_value(value));
}
