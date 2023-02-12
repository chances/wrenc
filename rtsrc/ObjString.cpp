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
	index = PrepareIndex(index, "Subscript");

	// Return the unicode codepoint, not the single byte!
	int length = GetUTF8Length(index);

	// If the string cuts off in the middle of a codepoint, return a single byte
	if (index + length > (int)m_value.size())
		return m_value.substr(index, 1);

	return m_value.substr(index, length);
}

int ObjString::CodePointAt_(int index) {
	index = PrepareIndex(index, "Index");

	uint8_t firstByte = (uint8_t)m_value[index];

	// Special-case ASCII so we don't have to deal with it later
	if ((firstByte & 0x80) == 0)
		return firstByte;

	// Return -1 if we're in the middle of a codepoint
	if ((firstByte & 0b11000000) == 0b10000000)
		return -1;

	int length = GetUTF8Length(index);

	// -1 if it's truncated by the end-of-string
	if (index + length > m_value.size())
		return -1;

	// The number of ones at the start indicates the number
	// of bytes in total, plus a following zero. Thus we can
	// figure out how many bits of the first byte are part
	// of the codepoint.
	int validInFirstByte = 8 - (length + 1);
	int codepoint = firstByte & ((1 << validInFirstByte) - 1);

	// Go through the remaining bytes
	for (int i = 1; i < length; i++) {
		uint8_t byte = (uint8_t)m_value.at(index + i);

		// This must be a continuation of the previous codepoint
		if ((byte & 0b11000000) != 0b10000000)
			return -1;

		codepoint = (codepoint << 6) | (byte & 0b00111111);
	}

	return codepoint;
}

int ObjString::ByteAt_(int index) {
	index = PrepareIndex(index, "Index");

	// Cast to unsigned so we don't get negatives
	return (uint8_t)m_value[index];
}

int ObjString::IndexOf(std::string target) const { return IndexOf(target, 0); }
int ObjString::IndexOf(std::string target, int startIndex) const {
	startIndex = PrepareIndex(startIndex, "Start");

	if (target.empty())
		return startIndex;

	size_t position = m_value.find(target, startIndex);
	if (position == std::string::npos)
		return -1;
	return position;
}

bool ObjString::Contains(std::string argument) const { return m_value.find(argument) != std::string::npos; }
bool ObjString::StartsWith(std::string argument) const {
	if (m_value.size() < argument.size())
		return false;
	return m_value.substr(0, argument.size()) == argument;
}
bool ObjString::EndsWith(std::string argument) const {
	if (m_value.size() < argument.size())
		return false;
	return m_value.substr(m_value.size() - argument.size()) == argument;
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
		// Walk forwards over the codepoint, unless it's truncated or
		// it's contents are invalid, in which case step through byte-by-byte.
		int len = GetUTF8Length(position);
		if (position + len > (int)m_value.size())
			len = 1;
		for (int i = 1; i < len; i++) {
			if ((m_value[position + i] & 0xc0) != 0x80)
				len = 1;
		}
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
		errors::wrenAbort("%s out of bounds.", argName);
	}
}

int ObjString::PrepareIndex(int index, const char *argName) const {
	// Negative indices count backwards
	if (index < 0) {
		index += m_value.size();
	}

	ValidateIndex(index, argName);
	return index;
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
