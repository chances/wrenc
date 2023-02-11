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

	WREN_METHOD(getter) static double Pi();

	WREN_METHOD(getter) double OperatorMinus(double receiver); // Negation
	WREN_METHOD(getter) bool OperatorBoolNegate(double receiver);
	WREN_METHOD(getter) double OperatorBitwiseNegate(double receiver);

	WREN_METHOD() double OperatorPlus(double receiver, double other);
	WREN_METHOD() double OperatorMinus(double receiver, double other);
	WREN_METHOD() double OperatorMultiply(double receiver, double other);
	WREN_METHOD() double OperatorDivide(double receiver, double other);

	// Bitwise binary operators
	WREN_METHOD() double OperatorAnd(double receiver, double other);
	WREN_METHOD() double OperatorOr(double receiver, double other);
	WREN_METHOD() double OperatorXOr(double receiver, double other);
	WREN_METHOD() double OperatorLeftShift(double receiver, double other);
	WREN_METHOD() double OperatorRightShift(double receiver, double other);

	WREN_METHOD() bool OperatorEqualTo(double receiver, Value other);
	WREN_METHOD() bool OperatorNotEqual(double receiver, Value other);
	WREN_METHOD() bool OperatorLessThan(double receiver, double other);
	WREN_METHOD() bool OperatorLessThanEq(double receiver, double other);
	WREN_METHOD() bool OperatorGreaterThan(double receiver, double other);
	WREN_METHOD() bool OperatorGreaterThanEq(double receiver, double other);

	WREN_METHOD() ObjRange *OperatorDotDot(double receiver, double other);
	WREN_METHOD() ObjRange *OperatorDotDotDot(double receiver, double other);

	// Trig functions
	WREN_METHOD(getter) double Sin(double receiver);
	WREN_METHOD(getter) double Cos(double receiver);
	WREN_METHOD(getter) double Tan(double receiver);
	WREN_METHOD(getter) double Asin(double receiver);
	WREN_METHOD(getter) double Acos(double receiver);
	WREN_METHOD(getter) double Atan(double receiver);
	WREN_METHOD() double Atan(double receiver, ARG("x value") double divisor); // atan2

	// Misc getter functions
	WREN_METHOD(getter) double Abs(double receiver);
	WREN_METHOD(getter) double Sqrt(double receiver);
	WREN_METHOD(getter) double Cbrt(double receiver);
	WREN_METHOD(getter) double Round(double receiver);
	WREN_METHOD(getter) double Floor(double receiver);
	WREN_METHOD(getter) double Ceil(double receiver);
	WREN_METHOD(getter) double Log(double receiver);
	WREN_METHOD(getter) double Log2(double receiver);
	WREN_METHOD(getter) double Sign(double receiver);
	WREN_METHOD(getter) double Fraction(double receiver);
	WREN_METHOD(getter) double Exp(double receiver);

	// Misc non-getter number functions
	WREN_METHOD() double Pow(double receiver, ARG("Power value") double power);
	WREN_METHOD() double Clamp(double receiver, ARG("Min value") double min, ARG("Max value") double max);
	WREN_METHOD() double Min(double receiver, double other);
	WREN_METHOD() double Max(double receiver, double other);
};
