//
// Created by znix on 22/07/22.
//

#include "ObjList.h"
#include "ObjBool.h"
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

void ObjList::ValidateIndex(int index, const char *argName) const {
	if (index < 0 || index >= (int)items.size()) {
		// TODO throw Wren error with this exact message
		fprintf(stderr, "%s out of bounds.\n", argName);
		abort();
	}
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
	if ((int)items.size() != index)
		ValidateIndex(index, "Index");

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
	if (current == NULL_VAL) {
		// Return zero to start iterating, but only if the list is not empty
		if (items.empty())
			return encode_object(ObjBool::Get(false));
		return 0;
	}

	if (is_object(current)) {
		// Already checked for null so this is safe
		std::string type = get_object_value(current)->type->name;
		fprintf(stderr, "Cannot supply object type %s to List.iterate(_)", type.c_str());
		abort();
	}

	int i = get_number_value(current);

	// If the index is invalid, we're supposed to return false. Only check the lower bound here, since
	// we'll check the upper bound after incrementing.
	if (i < 0)
		return encode_object(ObjBool::Get(false));

	i++;

	// Returning false stops the loop. Do this after incrementing i since the value we return will be passed
	// directly into items.at, which fails if i==items.size().
	if (i >= (int)items.size())
		return encode_object(ObjBool::Get(false));

	return encode_number(i);
}

Value ObjList::IteratorValue(int current) { return items.at(current); }

Value ObjList::OperatorSubscript(int index) {
	// Negative indices count backwards
	if (index < 0) {
		index += items.size();
	}

	ValidateIndex(index, "Subscript");
	return items.at(index);
}
