//
// Created by znix on 22/07/22.
//

#pragma once

#include "ObjClass.h"

#include <vector>

class ObjList : public Obj {
  public:
	ObjList();
	virtual ~ObjList();

	static ObjClass *Class();

	WREN_METHOD() static ObjList *New();

	WREN_METHOD() Value Add(Value toAdd);

	WREN_METHOD() std::string Join();
	WREN_METHOD() std::string Join(std::string joiner);

	std::vector<Value> items;
};
