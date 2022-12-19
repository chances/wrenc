//
// Created by znix on 24/07/22.
//

#include "ObjNum.h"

#include "Errors.h"
#include "ObjRange.h"
#include "WrenRuntime.h"

ObjNumClass::~ObjNumClass() {}

// Don't inherit methods from our parent, since we have the funny thing with the number receivers
ObjNumClass::ObjNumClass() : ObjNativeClass("Num", "ObjNumClass") {}

ObjNumClass *ObjNumClass::Instance() {
	static ObjNumClass cls;
	return &cls;
}

bool ObjNumClass::InheritsMethods() { return false; }

// Clang-tidy will think all these functions can be made static, which we don't want
// NOLINTBEGIN(readability-convert-member-functions-to-static)

bool ObjNumClass::Is(double receiver, ObjClass *cls) {
	// All numbers are pretending to be an instance of Num, and we're that instance.
	// Thus a simple pointer check will do.
	return cls == this;
}

std::string ObjNumClass::ToString(double receiver) {
	// Print integers as integers - passing a double into std::to_string will always
	// result in decimal points, even if the actual value is an integer.

	if ((int64_t)receiver == receiver) {
		return std::to_string((int64_t)receiver);
	}

	return std::to_string(receiver);
}

double ObjNumClass::OperatorMinus(double receiver) { return -receiver; }

bool ObjNumClass::OperatorBoolNegate(double receiver) {
	// Also return false, since numbers are 'truthy' values (same as everything except (IIRC) null and false).
	// Define here since Obj's methods aren't inherited.
	return false;
}

bool ObjNumClass::OperatorBitwiseNegate(double receiver) {
	// https://wren.io/modules/core/num.html
	// Truncate to a u32, negate, and cast back
	uint32_t intValue = (uint32_t)receiver;
	uint32_t negated = ~intValue;
	double result = (double)negated;
	return result;
}

double ObjNumClass::OperatorPlus(double receiver, double other) { return receiver + other; }
double ObjNumClass::OperatorMinus(double receiver, double other) { return receiver - other; }
double ObjNumClass::OperatorMultiply(double receiver, double other) { return receiver * other; }
double ObjNumClass::OperatorDivide(double receiver, double other) { return receiver / other; }

// Equals and not-equals are a bit different to the others - it's not an error to pass an object to them
bool ObjNumClass::OperatorEqualTo(double receiver, Value other) {
	if (is_object(other))
		return false;
	return receiver == get_number_value(other);
}
bool ObjNumClass::OperatorNotEqual(double receiver, Value other) { return !OperatorEqualTo(receiver, other); }

bool ObjNumClass::OperatorLessThan(double receiver, double other) { return receiver < other; }
bool ObjNumClass::OperatorLessThanEq(double receiver, double other) { return receiver <= other; }
bool ObjNumClass::OperatorGreaterThan(double receiver, double other) { return receiver > other; }
bool ObjNumClass::OperatorGreaterThanEq(double receiver, double other) { return receiver >= other; }

ObjRange *ObjNumClass::OperatorDotDot(double receiver, double other) {
	return WrenRuntime::Instance().New<ObjRange>(receiver, other, true);
}
ObjRange *ObjNumClass::OperatorDotDotDot(double receiver, double other) {
	return WrenRuntime::Instance().New<ObjRange>(receiver, other, false);
}

// NOLINTEND(readability-convert-member-functions-to-static)
