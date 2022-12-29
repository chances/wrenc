//
// Created by znix on 30/07/22.
//

#include "ObjRange.h"
#include "Errors.h"
#include "ObjBool.h"
#include "ObjClass.h"
#include "ObjNum.h"

class ObjRangeClass : public ObjNativeClass {
  public:
	ObjRangeClass() : ObjNativeClass("Range", "ObjRange") {}
};

ObjClass *ObjRange::Class() {
	static ObjRangeClass cls;
	return &cls;
}

ObjRange::ObjRange(double from, double to, bool inclusive) : Obj(Class()), from(from), to(to), inclusive(inclusive) {}

int ObjRange::From() const { return from; }
int ObjRange::To() const { return to; }
int ObjRange::Min() const { return std::min(from, to); }
int ObjRange::Max() const { return std::max(from, to); }
bool ObjRange::IsInclusive() const { return inclusive; }

Value ObjRange::Iterate(Value prev) const {
	// Note: this implementation is largely copied from Wren's wren_core.c

	// Empty ranges never iterate
	if (prev == to && !inclusive)
		return encode_object(ObjBool::Get(false));

	// Other than that, the starting case is simple enough
	if (prev == NULL_VAL)
		return encode_number(from);

	double current = errors::validateNum(prev, "Iterator");

	if (from < to) {
		// Iterating in the positive direction
		current++;

		if (current > to)
			return encode_object(ObjBool::Get(false));
	} else {
		// Iterating backwards
		current--;

		if (current < to)
			return encode_object(ObjBool::Get(false));
	}

	if (current == to && !inclusive)
		return encode_object(ObjBool::Get(false));

	return encode_number(current);
}
double ObjRange::IteratorValue(double value) const { return value; }

std::string ObjRange::ToString() const {
	std::string str = ObjNumClass::Instance()->ToString(from);
	if (inclusive)
		str.append(2, '.');
	else
		str.append(3, '.');
	str += ObjNumClass::Instance()->ToString(to);
	return str;
}

void ObjRange::MarkGCValues(GCMarkOps *ops) {
	// Nothing to be marked, only have numbers.
}
