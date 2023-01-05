//
// Created by znix on 10/07/2022.
//

#include "ObjMap.h"
#include "CoreClasses.h"
#include "Errors.h"
#include "ObjBool.h"
#include "ObjClass.h"
#include "ObjRange.h"
#include "ObjString.h"
#include "SlabObjectAllocator.h"

class ObjMapClass : public ObjNativeClass {
  public:
	ObjMapClass() : ObjNativeClass("Map", "ObjMap") {}
};

ObjClass *ObjMap::Class() {
	static ObjMapClass cls;
	return &cls;
}

ObjMap::ObjMap() : Obj(Class()) {}

ObjMap *ObjMap::New() { return SlabObjectAllocator::GetInstance()->AllocateNative<ObjMap>(); }

void ObjMap::Clear() { m_contents.clear(); }

bool ObjMap::ContainsKey(Value key) {
	ValidateKey(key);
	return m_contents.contains(key);
}

int ObjMap::Count() { return m_contents.size(); }

Value ObjMap::Remove(Value key) {
	ValidateKey(key);

	auto iter = m_contents.find(key);
	if (iter == m_contents.end()) {
		// Not in the map?
		return NULL_VAL;
	}
	Value value = iter->second;

	// Hopefully, erasing an iterator is faster than erasing by key.
	m_contents.erase(iter);

	return value;
}

Value ObjMap::OperatorSubscript(Value key) {
	ValidateKey(key);
	decltype(m_contents)::iterator iter = m_contents.find(key);
	if (iter == m_contents.end()) {
		return NULL_VAL;
	}
	return iter->second;
}

Value ObjMap::OperatorSubscriptSet(Value key, Value value) {
	ValidateKey(key);
	m_contents[key] = value;

	// I can't see how you'd access this, but looking at upstream Wren's implementation it
	// seems to return the value, so we'll copy that.
	return value;
}

Value ObjMap::Iterate(Value previous) {
	// If the map is empty, we're definitely done
	if (m_contents.empty()) {
		return encode_object(ObjBool::Get(false));
	}

	// Basically, we'll assume that unordered_map has a roughly stable order - obviously this completely
	// falls apart if we mutate it, and there might be some STL implementations where this breaks down.

	if (previous == NULL_VAL) {
		// Wrap the iterator value so it doesn't get recognised as null or false
		// Of course this means everything will explode if someone tries using it in Wren, but that's
		// fine - it's only ever passed into our key- and value-iterator-value functions.
		return WrapIterator(m_contents.begin()->first);
	}

	// A non-null iterator means it's already wrapped - so unwrap it
	Value unwrapped = UnwrapIterator(previous);

	// Lookup the iterator for this key
	auto iter = m_contents.find(unwrapped);
	if (iter == m_contents.end()) {
		// We should always find the item passed in as a key in the map, if not the map has probably been
		// modified while the loop was running.
		std::string keyStr = Obj::ToString(unwrapped);
		errors::wrenAbort("iterate() called with wrapped key '%s', which was not found in the map!", keyStr.c_str());
	}

	// Advance to the next item
	iter++;

	// Was the previous item the last one? Then stop iterating.
	if (iter == m_contents.end()) {
		return encode_object(ObjBool::Get(false));
	}

	// Otherwise, just return it's key (but properly wrapped).
	return WrapIterator(iter->first);
}
Value ObjMap::KeyIteratorValue_(Value iterator) { return UnwrapIterator(iterator); }
Value ObjMap::ValueIteratorValue_(Value iterator) {
	auto iter = m_contents.find(UnwrapIterator(iterator));
	if (iter == m_contents.end()) {
		errors::wrenAbort("Called valueIteratorValue_ with an invalid iterator - not found in map!");
	}
	return iter->second;
}

Value ObjMap::WrapIterator(Value raw) {
	// We have to make sure that iterator values are always 'true'-like so iteration doesn't end if
	// we run into a null or false value. To do this we'll just increment all pointers by 1.
	// Note we receive special GC support for this, in GCTracingScanner::AddToGreyList.

	// Number values are always fine
	if (is_value_float(raw))
		return raw;

	uint64_t ptr = (uint64_t)get_object_value(raw);
	Obj *wrapped = (Obj *)(ptr + 1);
	return encode_object(wrapped);
}
Value ObjMap::UnwrapIterator(Value wrapped) {
	// Opposite of WrapIterator
	if (is_value_float(wrapped))
		return wrapped;

	uint64_t ptr = (uint64_t)get_object_value(wrapped);
	Obj *raw = (Obj *)(ptr - 1);
	return encode_object(raw);
}

void ObjMap::ValidateKey(Value key) {
	// Floats and nulls are fine
	if (is_value_float(key) || key == NULL_VAL)
		return;

	Obj *obj = get_object_value(key);

	// As are strings, bools and ranges
	if (obj->type == ObjString::Class())
		return;
	if (obj->type == ObjBool::Class())
		return;
	if (obj->type == ObjRange::Class())
		return;

	// If the object passed in is a class - that is, it extends from ObjClass - then it's allowed.
	if (obj->Is(&CoreClasses::Instance()->RootClass()))
		return;

	errors::wrenAbort("Key must be a value type.");
}

void ObjMap::MarkGCValues(GCMarkOps *ops) {
	for (const auto &[key, value] : m_contents) {
		ops->ReportValue(ops, key);
		ops->ReportValue(ops, value);
	}
}

std::size_t simpleHash(Value value) {
	// No particular logic in this, other than picking primes because that's usually a good idea?
	return (value * 23) ^ ((11 * value) << 7) ^ (value >> 31);
}

std::size_t ObjMap::ValueHash::operator()(Value value) const noexcept {
	// For numeric values, scramble them a bit
	if (!is_object(value)) {
		return simpleHash(value);
	}

	// https://wren.io/maps.html
	// There's a few possible key types:
	// Numbers, null, string, range, bool, or a class type.
	// For all of these except strings and ranges, we can just mush up the pointer value and use
	// that as a hash.
	// TODO implement range support

	Obj *obj = get_object_value(value);
	if (obj == nullptr)
		return simpleHash(value);

	// Handle strings by using the STL's hash function
	if (obj->type == ObjString::Class()) {
		ObjString *str = (ObjString *)obj;
		return std::hash<std::string>{}(str->m_value);
	}

	// Otherwise, just hash the value
	return simpleHash(value);
}

bool ObjMap::ValueEqual::operator()(Value a, Value b) const noexcept {
	// Numeric values are simple
	if (!is_object(a))
		return a == b;

	// If one is an object but not the other, that's also simple
	if (is_object(a) != is_object(b))
		return false;

	// At this point, both values must be objects
	Obj *objA = get_object_value(a);
	Obj *objB = get_object_value(b);

	// Handle nulls first, so we can dereference the variables
	if (objA == nullptr)
		return objB == nullptr;

	// Compare strings by value, not identity
	// Note that if B is a string but not A, they'll be compared by identity and fail that way
	if (objA->type == ObjString::Class() && objB->type == ObjString::Class()) {
		ObjString *strA = (ObjString *)objA;
		ObjString *strB = (ObjString *)objB;
		return strA->m_value == strB->m_value;
	}

	// TODO implement range support

	// Assume all other values are equal by identity
	return objA == objB;
}
