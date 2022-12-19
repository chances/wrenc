//
// Created by znix on 19/12/22.
//

#pragma once

#include "common.h"

#include <unordered_map>
#include <string>

/// Represents, and stores data about, a module at runtime
class RtModule {
  public:
	RtModule(void *globalsTable);

	/// This module's global variables
	std::unordered_map<std::string, Value *> globals;
};
