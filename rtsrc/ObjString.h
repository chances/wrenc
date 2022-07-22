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

	std::string m_value;
};
