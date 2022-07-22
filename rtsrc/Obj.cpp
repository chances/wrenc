//
// Created by znix on 10/07/2022.
//

#include "Obj.h"
#include "CoreClasses.h"
#include "ObjClass.h"
#include "ObjString.h"

#include <string>

Obj::~Obj() = default;

std::string Obj::ToString() {
	static SignatureId SIG = ObjClass::FindSignatureId("toString");
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

	return nullptr;
}
