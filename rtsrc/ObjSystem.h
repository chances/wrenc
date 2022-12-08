//
// Created by znix on 21/07/22.
//

#pragma once

#include "ObjClass.h"

class ObjSystem : public ObjNativeClass {
  public:
	ObjSystem();

	WREN_METHOD() static void WriteString_(std::string value);
};
