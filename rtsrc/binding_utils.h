//
// Utility functions used in the generated binding file
//
// Created by znix on 22/07/22.
//

#pragma once

#include "Errors.h"
#include "Obj.h"
#include "ObjClass.h"
#include "ObjString.h"
#include "common.h"

#include <math.h>

double checkInt(const char *method, int arg, Value value);
double checkDouble(const char *method, int arg, Value value);
std::string checkString(const char *method, int arg, Value value);

template <typename T> T *checkReceiver(const char *method, Value value) {
	if (!is_object(value)) {
		errors::wrenAbort("Native function %s: receiver is not an object!", method);
	}

	Obj *obj = (Obj *)get_object_value(value);
	if (!obj) {
		errors::wrenAbort("Native function %s: receiver is null!", method);
	}

	T *casted = dynamic_cast<T *>(obj);
	if (!casted) {
		errors::wrenAbort("Native function %s: receiver is invalid type '%s'!", method, obj->type->name.c_str());
	}

	return casted;
}

template <typename T> T *checkArg(const char *method, int arg, Value value, bool nullable) {
	if (!is_object(value)) {
		errors::wrenAbort("Native function %s: argument %d is not an object!", method, arg);
	}

	Obj *obj = (Obj *)get_object_value(value);
	if (!obj) {
		if (nullable)
			return nullptr;
		errors::wrenAbort("Native function %s: argument %d is null!", method, arg);
	}

	T *casted = dynamic_cast<T *>(obj);
	if (!casted) {
		errors::wrenAbort("Native function %s: argument %d is invalid type '%s'!", method, arg,
		    obj->type->name.c_str());
	}

	return casted;
}

// Put the implementations into the generated file, so they can be inlined
#ifdef BINDINGS_GEN

std::string checkString(const char *method, int arg, Value value) {
	ObjString *str = checkArg<ObjString>(method, arg, value, false);
	return str->m_value;
}

double checkDouble(const char *method, int arg, Value value) {
	if (!is_value_float(value)) {
		errors::wrenAbort("Native function %s: argument %d is not a number!", method, arg);
	}

	return get_number_value(value);
}

double checkInt(const char *method, int arg, Value value) {
	double num = checkDouble(method, arg, value);
	int intValue = floor(num);
	if (intValue != num) {
		errors::wrenAbort(
		    "Native function %s: argument %d is a floating point value '%f', not an integer or is too large", method,
		    arg, num);
	}

	return intValue;
}

#endif
