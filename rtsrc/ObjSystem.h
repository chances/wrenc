//
// Created by znix on 21/07/22.
//

#pragma once

#include "ObjClass.h"

class ObjSystem : public Obj {
  public:
	ObjSystem();

	static ObjClass *Class();

	void MarkGCValues(GCMarkOps *ops) override;

	WREN_METHOD() static void WriteString_(std::string value);

	WREN_METHOD(getter) static double Clock();

	WREN_METHOD() static void Gc();
};
