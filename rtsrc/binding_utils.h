//
// Utility functions used in the generated binding file
//
// Created by znix on 22/07/22.
//

#pragma once

#include "Errors.h"
#include "Obj.h"
#include "ObjClass.h"
#include "ObjFn.h"
#include "ObjString.h"
#include "common/common.h"

#include <math.h>

double checkInt(const char *method, const char *errorName, int arg, Value value);
double checkDouble(const char *method, const char *errorName, int arg, Value value);
std::string checkString(const char *method, const char *errorName, int arg, Value value);

template <typename T> void throwArgTypeError(const char *errorName);

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

template <typename T> T *checkArg(const char *method, const char *errorName, int arg, Value value, bool nullable) {
	if (!is_object(value)) {
		throwArgTypeError<T>(errorName);
	}

	Obj *obj = (Obj *)get_object_value(value);
	if (!obj) {
		if (nullable)
			return nullptr;
		throwArgTypeError<T>(errorName);
	}

	T *casted = dynamic_cast<T *>(obj);
	if (!casted) {
		throwArgTypeError<T>(errorName);
	}

	return casted;
}

// Put the implementations into the generated file, so they can be inlined
#ifdef BINDINGS_GEN

std::string checkString(const char *method, const char *errorName, int arg, Value value) {
	ObjString *str = checkArg<ObjString>(method, errorName, arg, value, false);
	return str->m_value;
}

double checkDouble(const char *method, const char *errorName, int arg, Value value) {
	if (!is_value_float(value)) {
		errors::wrenAbort("%s must be a number.", errorName);
	}

	return get_number_value(value);
}

double checkInt(const char *method, const char *errorName, int arg, Value value) {
	double num = checkDouble(method, errorName, arg, value);
	int intValue = floor(num);
	if (intValue != num) {
		errors::wrenAbort("%s must be an integer.", errorName);
	}

	return intValue;
}

template <> void throwArgTypeError<ObjString>(const char *errorName) {
	errors::wrenAbort("%s must be a string.", errorName);
}

template <> void throwArgTypeError<ObjFn>(const char *errorName) {
	errors::wrenAbort("%s must be a function.", errorName);
}

template <> void throwArgTypeError<ObjClass>(const char *errorName) {
	errors::wrenAbort("%s must be a class.", errorName);
}

#endif
