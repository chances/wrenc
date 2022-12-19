//
// Created by znix on 24/07/22.
//

#pragma once

#include "ObjClass.h"

/// The metaclass that we pretend numbers have. Since numbers aren't actual objects, this is all going to be
/// a bit weird and a bit magic with lots of help from the compiler to make this illusion work.
/// Note this class name is used in gen_bindings.py, be sure to change it if you rename this class.
class ObjNumClass : public ObjNativeClass {
  public:
	~ObjNumClass();
	ObjNumClass();

	bool InheritsMethods() override;

	static ObjNumClass *Instance();

	WREN_METHOD() bool Is(double receiver, ObjClass *cls);
	WREN_METHOD(getter) std::string ToString(double receiver);

	WREN_METHOD(getter) double OperatorMinus(double receiver); // Negation
	WREN_METHOD(getter) bool OperatorBoolNegate(double receiver);
	WREN_METHOD(getter) bool OperatorBitwiseNegate(double receiver);

	WREN_METHOD() double OperatorPlus(double receiver, double other);
	WREN_METHOD() double OperatorMinus(double receiver, double other);
	WREN_METHOD() double OperatorMultiply(double receiver, double other);
	WREN_METHOD() double OperatorDivide(double receiver, double other);

	WREN_METHOD() bool OperatorEqualTo(double receiver, Value other);
	WREN_METHOD() bool OperatorNotEqual(double receiver, Value other);
	WREN_METHOD() bool OperatorLessThan(double receiver, double other);
	WREN_METHOD() bool OperatorLessThanEq(double receiver, double other);
	WREN_METHOD() bool OperatorGreaterThan(double receiver, double other);
	WREN_METHOD() bool OperatorGreaterThanEq(double receiver, double other);

	WREN_METHOD() ObjRange *OperatorDotDot(double receiver, double other);
	WREN_METHOD() ObjRange *OperatorDotDotDot(double receiver, double other);
};
