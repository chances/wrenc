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

void ObjList::ValidateIndex(int index) {
	if (index < 0 || index >= (int)items.size())
		abort();
}

ObjList *ObjList::New() { return WrenRuntime::Instance().New<ObjList>(); }

Value ObjList::Add(Value toAdd) {
	items.push_back(toAdd);
	return toAdd;
}

Value ObjList::Insert(int index, Value toAdd) {
	// Negative index handling: -1 means size (so same as Add), -2 means size-1 etc
	if (index < 0) {
		index = items.size() + 1 + index;
	}

	// Inserting an item at the end is fine
	if (items.size() != index)
		ValidateIndex(index);

	items.insert(items.begin() + index, toAdd);
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

Value ObjList::Iterate(Value current) {
	if (current == NULL_VAL)
		return 0;

	if (is_object(current)) {
		// Already checked for null so this is safe
		std::string type = get_object_value(current)->type->name;
		fprintf(stderr, "Cannot supply object type %s to List.iterate(_)", type.c_str());
		abort();
	}

	int i = get_number_value(current);
	i++;

	// Returning null stops the loop. Do this after incrementing i since the value we return will be passed
	// directly into items.at, which fails if i==items.size().
	if (i >= (int)items.size())
		return NULL_VAL;

	return encode_number(i);
}

Value ObjList::IteratorValue(int current) { return items.at(current); }
