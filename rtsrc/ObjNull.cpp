//
// Created by znix on 30/07/22.
//

#include "ObjNull.h"
#include "ObjClass.h"

class ObjNullClass : public ObjNativeClass {
  public:
	ObjNullClass() : ObjNativeClass("Null", "ObjNull") {}
};

ObjClass *ObjNull::Class() {
	static ObjNullClass cls;
	return &cls;
}

std::string ObjNull::ToString() { return "null"; }

bool ObjNull::OperatorEqualTo(Value other) { return other == NULL_VAL; }
bool ObjNull::OperatorNotEqual(Value other) { return other != NULL_VAL; }
bool ObjNull::OperatorBoolNegate() {
	// !null is always true
	return true;
}
bool ObjNull::Is(ObjClass *cls) {
	// We want the regular behaviour from Is, just with a different receiver
	return Obj::Is(cls);
}
