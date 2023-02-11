//
// Created by znix on 10/07/2022.
//

#include "ObjString.h"
#include "Errors.h"
#include "ObjBool.h"
#include "SlabObjectAllocator.h"

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
	ObjString *obj = SlabObjectAllocator::GetInstance()->AllocateNative<ObjString>();
	obj->m_value = value;
	return obj;
}

ObjString *ObjString::New(std::string &&value) {
	ObjString *obj = SlabObjectAllocator::GetInstance()->AllocateNative<ObjString>();
	obj->m_value = std::move(value);
	return obj;
}

ObjString *ObjString::FromCodePoint(int codepoint) {
	ObjString *str = New("");
	str->AppendCodepoint(codepoint);
	return str;
}

ObjString *ObjString::FromByte(int byte) {
	ObjString *str = New("");
	str->m_value.push_back(byte);
	return str;
}

Value ObjString::ToString() { return encode_object(this); }

int ObjString::ByteCount_() { return m_value.size(); }

std::string ObjString::OperatorPlus(std::string other) { return m_value + other; }

std::string ObjString::OperatorSubscript(int index) {
	// Negative indices count backwards
	if (index < 0) {
		index += m_value.size();
	}

	ValidateIndex(index, "Subscript");

	// Return the unicode codepoint, not the single byte!
	int length = GetUTF8Length(index);

	// If the string cuts off in the middle of a codepoint, return a single byte
	if (index + length > (int)m_value.size())
		return m_value.substr(index, 1);

	return m_value.substr(index, length);
}

int ObjString::ByteAt_(int index) {
	ValidateIndex(index, "Index");

	// Cast to unsigned so we don't get negatives
	return (uint8_t)m_value[index];
}

Value ObjString::IterateImpl(Value previous, bool unicode) const {
	// Empty strings are obviously empty
	if (m_value.empty())
		return encode_object(ObjBool::Get(false));

	// First iteration? Start at the start.
	if (previous == NULL_VAL)
		return encode_number(0);

	int position = errors::validateInt(previous, "Iterator");

	if (position < 0)
		return encode_object(ObjBool::Get(false)); // Invalid, specified by the iterate.wren test

	if (unicode) {
		// Walk forwards over the codepoint, unless it's truncated, in
		// which case step through byte-by-byte.
		int len = GetUTF8Length(position);
		if (position + len > (int)m_value.size())
			len = 1;
		position += len;
	} else {
		position++;
	}

	if (position >= (int)m_value.size())
		return encode_object(ObjBool::Get(false));

	return encode_number(position);
}

Value ObjString::Iterate(Value previous) { return IterateImpl(previous, true); }

Value ObjString::IterateByte_(Value previous) { return IterateImpl(previous, false); }

std::string ObjString::IteratorValue(int iterator) {
	// Note the values from iterateByte_ are only used in wren_core by StringByteSequence, and they're
	// passed into byteAt_ - so IteratorValue doesn't have to care about them.
	return OperatorSubscript(iterator);
}

void ObjString::ValidateIndex(int index, const char *argName) const {
	if (index < 0 || index >= (int)m_value.size()) {
		// TODO throw Wren error with this exact message
		errors::wrenAbort("%s out of bounds.", argName);
	}
}

int ObjString::GetUTF8Length(int index) const {
	uint8_t c = m_value[index];

	// ASCII characters have the top bit clear
	if ((c & 0x80) == 0)
		return 1;

	if ((c & 0b11100000) == 0b11000000)
		return 2;
	if ((c & 0b11110000) == 0b11100000)
		return 3;
	if ((c & 0b11111000) == 0b11110000)
		return 4;

	// Anything else is broken, just assume it's a single byte.
	// This can happen if we cut into the middle of a string.
	return 1;
}

void ObjString::MarkGCValues(GCMarkOps *ops) {
	// Nothing to do.
}

bool ObjString::EqualTo(Obj *other) {
	ObjString *str = dynamic_cast<ObjString *>(other);
	if (str == nullptr)
		return false;
	return str->m_value == m_value;
}

void ObjString::AppendCodepoint(int codepoint) {
	if (codepoint < 0)
		errors::wrenAbort("Cannot append negative codepoint to string: %d", codepoint);

	// Simple UTF-8 encoding
	if (codepoint < 0x80) {
		m_value.push_back((char)codepoint);
	} else if (codepoint < 0x800) {
		m_value.push_back(0b11000000 | (codepoint >> 6));
		m_value.push_back(0b10000000 | (codepoint & 0x3f));
	} else if (codepoint < 0x10000) {
		m_value.push_back(0b11100000 | (codepoint >> 12));
		m_value.push_back(0b10000000 | ((codepoint >> 6) & 0x3f));
		m_value.push_back(0b10000000 | (codepoint & 0x3f));
	} else if (codepoint < 0x110000) {
		m_value.push_back(0b11110000 | (codepoint >> 18));
		m_value.push_back(0b10000000 | ((codepoint >> 12) & 0x3f));
		m_value.push_back(0b10000000 | ((codepoint >> 6) & 0x3f));
		m_value.push_back(0b10000000 | (codepoint & 0x3f));
	} else {
		errors::wrenAbort("Codepoint too large: %d", codepoint);
	}
}
