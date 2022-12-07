//
// Created by znix on 10/07/2022.
//

#include "ObjString.h"
#include "WrenRuntime.h"

class ObjStringClass : public ObjNativeClass {
  public:
	ObjStringClass() : ObjNativeClass("String", "ObjString") {}
};

ObjNativeClass *ObjString::Class() {
	static ObjStringClass type;
	return &type;
}

ObjString::ObjString() : Obj(Class()) {}

ObjString *ObjString::New(const std::string &value) {
	ObjString *obj = WrenRuntime::Instance().New<ObjString>();
	obj->m_value = value;
	return obj;
}

ObjString *ObjString::New(std::string &&value) {
	ObjString *obj = WrenRuntime::Instance().New<ObjString>();
	obj->m_value = std::move(value);
	return obj;
}

Value ObjString::ToString() { return encode_object(this); }

int ObjString::Count() { return m_value.size(); }

std::string ObjString::OperatorPlus(Value other) { return m_value + Obj::ToString(other); }
