//
// Created by znix on 10/07/2022.
//

#pragma once

#include <string>

#include "Obj.h"
#include "ObjClass.h"

class ObjString : public Obj {
  public:
	static ObjNativeClass *Class();

	ObjString();

	static ObjString *New(const std::string &value);
	static ObjString *New(std::string &&value);

	void MarkGCValues(GCMarkOps *ops) override;
	bool EqualTo(Obj *other) override;

	void AppendCodepoint(int codepoint);

	WREN_METHOD() static ObjString *FromCodePoint(ARG("Code point") int codepoint);
	WREN_METHOD() static ObjString *FromByte(int byte);

	WREN_METHOD(getter) Value ToString();
	// Maybe we should implement Count for performance? Otherwise wren_core does it.
	WREN_METHOD(getter) int ByteCount_();
	WREN_METHOD() std::string OperatorPlus(ARG("Right operand") std::string other);
	WREN_METHOD() std::string OperatorSubscript(Value indexOrRange);
	WREN_METHOD() int CodePointAt_(int index);
	WREN_METHOD() int ByteAt_(int index);

	WREN_METHOD() int IndexOf(std::string argument) const;
	WREN_METHOD() int IndexOf(std::string argument, int start) const;
	WREN_METHOD() bool Contains(std::string argument) const;
	WREN_METHOD() bool StartsWith(std::string argument) const;
	WREN_METHOD() bool EndsWith(std::string argument) const;

	WREN_METHOD() Value Iterate(Value previous);
	WREN_METHOD() Value IterateByte_(Value previous);
	WREN_METHOD() std::string IteratorValue(int iterator);

	std::string m_value;

  private:
	Value IterateImpl(Value previous, bool unicode) const;

	void ValidateIndex(int index, const char *argName, bool exclusive = false) const;

	// Handles both ValidateIndex and wrap-arounds from negative indexes
	int PrepareIndex(int index, const char *argName, bool exclusive = false) const;

	// Find the byte length of the UTF-8 codepoint at the given index
	int GetUTF8Length(int index) const;
};
