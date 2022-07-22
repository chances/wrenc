//
// Created by znix on 22/07/22.
//

#include "ObjBool.h"
#include "ObjClass.h"

class ObjBoolClass : public ObjNativeClass {
  public:
	ObjBoolClass() : ObjNativeClass("Bool", "ObjBool") {}
};

ObjBool::ObjBool(bool value) : Obj(Class()), m_value(value) {}

ObjClass *ObjBool::Class() {
	static ObjBoolClass cls;
	return &cls;
}
