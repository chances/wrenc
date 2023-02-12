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
bool ObjNull::Is(ObjClass *cls) { return Class()->Extends(cls); }

void ObjNull::MarkGCValues(GCMarkOps *ops) {
	// Never called - it can't be, since no instances of this class are ever created :)
	abort();
}

ObjClass *ObjNull::Type() { return Class(); }
