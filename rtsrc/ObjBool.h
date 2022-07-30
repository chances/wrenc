//
// Created by znix on 22/07/22.
//

#pragma once

#include "Obj.h"

/// Represents a boolean value. Unlike normal wren, booleans are real objects for performance
/// reasons (specifically to avoid having a third class of objects beyond pointers and numbers).
/// Only two objects are ever created, and no prises for guessing what they represent.
class ObjBool : public Obj {
  public:
	inline bool AsBool() const { return m_value; }

	static ObjBool *Get(bool value) {
		static ObjBool objTrue(true), objFalse(false);
		return value ? &objTrue : &objFalse;
	}

	static ObjClass *Class();

	WREN_METHOD(getter) std::string ToString();
	WREN_METHOD(getter) bool OperatorBoolNegate();

  private:
	ObjBool(bool value);
	bool m_value = false;
};
