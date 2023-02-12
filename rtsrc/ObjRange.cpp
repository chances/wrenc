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

double ObjRange::From() const { return from; }
double ObjRange::To() const { return to; }
double ObjRange::Min() const { return std::min(from, to); }
double ObjRange::Max() const { return std::max(from, to); }
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

void ObjRange::ToSubscriptUtil(int length, int &start, int &end, bool &reverse) const {
	reverse = false;

	start = (int)from;
	end = (int)to;

	if (start != from)
		errors::wrenAbort("Range start must be an integer.");
	if (end != to)
		errors::wrenAbort("Range end must be an integer.");

	if (start < 0)
		start += length;

	// It's legal to ask for a zero-length string at the end of any string.
	// Handle it here so we don't have to modify our bounds-checking for it.
	// Note that we have to do this before fixing up the end-range value, as
	// for some reason only -1 is allowed for non-inclusive ranges.
	// See wren_primitive.c calculateRange for Wren's implementation of this.
	if (start == length && end == (inclusive ? -1 : length)) {
		start = 0;
		end = 0;
		return;
	}

	if (end < 0)
		end += length;

	bool isEmptyRange = !inclusive && start == end;

	// Make end inclusive. Note that if the range is 'backwards' (where
	// end<start), then if you count backwards and stop earlier or later
	// it'll be the opposite of if you're counting forwards, hence the
	// need to treat these two cases differently.
	if (!inclusive) {
		if (start <= end)
			end--;
		else
			end++;
	}

	// Handle the range going backwards - if so, we'll later reverse the result.
	// We have to be careful not to make it impossible to express empty ranges here, though.
	reverse = start > end && !isEmptyRange;
	if (reverse) {
		std::swap(start, end);
	}

	// Our range is currently inclusive, since that makes it easier when we're
	// dealing with reversing the string. Now we've decided which value is
	// the end, increment it to make the range exclusive.
	end++;

	// Perform the range check. Annoyingly, since we reverse the start/end earlier
	// we also have to reverse the error names here, so the error is attributed to
	// the correct side of the range.
	const char *startName = "Range start";
	const char *endName = "Range end";
	if (reverse) {
		std::swap(startName, endName);
	}
	if (start < 0 || start >= length) {
		errors::wrenAbort("%s out of bounds.", startName);
	}
	if (end < 0 || end > length) {
		errors::wrenAbort("%s out of bounds.", endName);
	}
}
