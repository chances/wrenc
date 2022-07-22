//
// Utility functions used in the generated binding file
//
// Created by znix on 22/07/22.
//

#pragma once

#include "Obj.h"
#include "ObjClass.h"
#include "common.h"

template <typename T> T *checkReceiver(const char *method, Value value) {
	if (!is_object(value)) {
		fprintf(stderr, "Native function %s: receiver is not an object!\\n", method);
		abort();
	}

	Obj *obj = (Obj *)get_object_value(value);
	if (!obj) {
		fprintf(stderr, "Native function %s: receiver is null!\\n", method);
		abort();
	}

	T *casted = dynamic_cast<T *>(obj);
	if (!casted) {
		fprintf(stderr, "Native function %s: receiver is invalid type '%s'!\\n", method, obj->type->name.c_str());
		abort();
	}

	return casted;
}
