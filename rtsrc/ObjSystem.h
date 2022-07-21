//
// Created by znix on 21/07/22.
//

#pragma once

#include "ObjClass.h"

// Because ObjSystem is a singleton, it can't actually have instances, so we only need a metaclass.
class ObjSystem : public ObjNativeClass {
  public:
	ObjSystem();
};
