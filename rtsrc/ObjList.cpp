//
// Created by znix on 22/07/22.
//

#include "ObjList.h"
#include "ObjClass.h"
#include "WrenRuntime.h"

#include <sstream>

class ObjListClass : public ObjNativeClass {
  public:
	ObjListClass() : ObjNativeClass("List", "ObjList") {}
};

ObjList::ObjList() : Obj(Class()) {}
ObjList::~ObjList() {}

ObjClass *ObjList::Class() {
	static ObjListClass cls;
	return &cls;
}

ObjList *ObjList::New() { return WrenRuntime::Instance().New<ObjList>(); }

Value ObjList::Add(Value toAdd) {
	items.push_back(toAdd);
	return toAdd;
}

std::string ObjList::Join() { return Join(""); }
std::string ObjList::Join(std::string joiner) {
	std::stringstream result;
	bool first = true;
	for (Value value : items) {
		if (!first)
			result << joiner;
		first = false;
		result << Obj::ToString(value);
	}
	return result.str();
}
