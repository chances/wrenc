//
// Created by znix on 30/07/22.
//

#pragma once

#include "Obj.h"

/// This doesn't have any special C++ functionality, it's just a common supertype that some C++ classes extend, and
/// thus itself has to live in C++.
class ObjSequence : public Obj {
  public:
	static ObjClass *Class();

	// Be clear that this is still an abstract type.
	void MarkGCValues(GCMarkOps *ops) = 0;
};
