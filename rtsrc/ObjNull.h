//
// Created by znix on 30/07/22.
//

#pragma once

#include "Obj.h"

/// A class on which to declare the methods of the 'null object'. No instances of this
/// class are ever constructed, and it only exists to be picked up by the method binding
/// script. It's class representation is however used when attempting to call a method
/// on a null object.
class ObjNull : public Obj {
  public:
	ObjNull() = delete;

	static ObjClass *Class();

	void MarkGCValues(GCMarkOps *ops) override;

	WREN_METHOD(getter) std::string ToString();

	// Reimplement some basic operators, since the normal Obj ones won't work with null receivers
	WREN_METHOD() bool OperatorEqualTo(Value other);
	WREN_METHOD() bool OperatorNotEqual(Value other);
	WREN_METHOD(getter) bool OperatorBoolNegate();
	WREN_METHOD() bool Is(ARG("Right operand") ObjClass *cls);
	WREN_METHOD(getter) ObjClass *Type();
};
