//
// Created by znix on 22/07/22.
//

#include "ObjList.h"
#include "Errors.h"
#include "ObjBool.h"
#include "ObjClass.h"
#include "ObjRange.h"
#include "SlabObjectAllocator.h"
#include "binding_utils.h"

#include <algorithm>
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

void ObjList::MarkGCValues(GCMarkOps *ops) { ops->ReportValues(ops, items.data(), items.size()); }

void ObjList::ValidateIndex(int index, const char *argName) const {
	if (index < 0 || index >= (int)items.size()) {
		// TODO throw Wren error with this exact message
		errors::wrenAbort("%s out of bounds.", argName);
	}
}

ObjList *ObjList::New() { return SlabObjectAllocator::GetInstance()->AllocateNative<ObjList>(); }

ObjList *ObjList::Filled(int size, Value element) {
	if (size < 0) {
		errors::wrenAbort("Size cannot be negative.");
	}
	ObjList *list = New();
	list->items.assign(size, element);
	return list;
}

void ObjList::Clear() { items.clear(); }

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

Value ObjList::Remove(Value toRemove) {
	int index = IndexOf(toRemove);
	if (index == -1)
		return NULL_VAL;
	return RemoveAt(index);
}

Value ObjList::RemoveAt(int index) {
	// Negative index handling: -1 means last item (which is size-1), -2=size-2 etc
	if (index < 0) {
		index = items.size() + index;
	}

	ValidateIndex(index, "Index");

	Value previous = items[index];
	items.erase(items.begin() + index);
	return previous;
}

int ObjList::IndexOf(Value toFind) {
	auto iter = std::find(items.begin(), items.end(), toFind);

	if (iter == items.end())
		return -1;

	return iter - items.begin();
}

Value ObjList::Iterate(Value current) {
	if (current == NULL_VAL) {
		// Return zero to start iterating, but only if the list is not empty
		if (items.empty())
			return encode_object(ObjBool::Get(false));
		return 0;
	}

	int i = checkInt(nullptr, "Iterator", 0, current);

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

Value ObjList::IteratorValue(int current) {
	ValidateIndex(current, "Iterator");
	return items.at(current);
}

Value ObjList::OperatorSubscript(Value indexOrRange) {
	// Range support
	if (is_object(indexOrRange)) {
		ObjRange *range = dynamic_cast<ObjRange *>(get_object_value(indexOrRange));

		if (!range) {
			errors::wrenAbort("Subscript must be a number or a range.");
		}

		int start, end;
		bool reversed;
		range->ToSubscriptUtil(items.size(), start, end, reversed);

		ObjList *slice = New();
		slice->items.assign(items.begin() + start, items.begin() + end);

		// Reverse the contents if necessary
		if (reversed) {
			std::reverse(slice->items.begin(), slice->items.end());
		}

		return encode_object(slice);
	}

	double num = get_number_value(indexOrRange);
	int index = (int)num;
	if (index != num) {
		errors::wrenAbort("Subscript must be an integer.");
	}

	// Negative indices count backwards
	if (index < 0) {
		index += items.size();
	}

	ValidateIndex(index, "Subscript");
	return items.at(index);
}

Value ObjList::OperatorSubscriptSet(int index, Value value) {
	// Negative indices count backwards
	if (index < 0) {
		index += items.size();
	}

	ValidateIndex(index, "Subscript");
	items.at(index) = value;
	return value;
}

int ObjList::Count() const { return items.size(); }
