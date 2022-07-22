//
// Created by znix on 22/07/22.
//

#pragma once

#include "Obj.h"

#include <vector>

class ObjList : public Obj {
  public:
	ObjList();
	virtual ~ObjList();

	static ObjClass *Class();

	std::vector<Value> items;
};
