//
// Created by znix on 24/07/22.
//

#include "ObjNum.h"

ObjNumClass::~ObjNumClass() {}

// Don't inherit methods from our parent, since we have the funny thing with the number receivers
ObjNumClass::ObjNumClass() : ObjNativeClass("Num", "ObjNumClass", nullptr, false) {}

ObjNumClass *ObjNumClass::Instance() {
	static ObjNumClass cls;
	return &cls;
}

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

double ObjNumClass::OperatorPlus(double receiver, double other) { return receiver + other; }
double ObjNumClass::OperatorMinus(double receiver, double other) { return receiver - other; }
double ObjNumClass::OperatorMultiply(double receiver, double other) { return receiver * other; }
double ObjNumClass::OperatorDivide(double receiver, double other) { return receiver / other; }

bool ObjNumClass::OperatorEqualTo(double receiver, double other) { return receiver == other; }
bool ObjNumClass::OperatorNotEqual(double receiver, double other) { return receiver != other; }
bool ObjNumClass::OperatorLessThan(double receiver, double other) { return receiver < other; }
bool ObjNumClass::OperatorLessThanEq(double receiver, double other) { return receiver <= other; }
bool ObjNumClass::OperatorGreaterThan(double receiver, double other) { return receiver > other; }
bool ObjNumClass::OperatorGreaterThanEq(double receiver, double other) { return receiver >= other; }

// NOLINTEND(readability-convert-member-functions-to-static)