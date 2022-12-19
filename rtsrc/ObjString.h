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

	WREN_METHOD(getter) Value ToString();
	WREN_METHOD(getter) int Count();
	WREN_METHOD() std::string OperatorPlus(Value other);
	WREN_METHOD() std::string OperatorSubscript(int index);

	std::string m_value;

  private:
	void ValidateIndex(int index, const char *argName) const;
};
