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
