//
// Created by znix on 19/12/22.
//

#pragma once

#include "common.h"

#include <string>
#include <unordered_map>

/// Represents, and stores data about, a module at runtime
class RtModule {
  public:
	RtModule(void *globalsTable);

	/// Look up a global by varName, returning null if one isn't found
	Value *GetOrNull(const std::string &varName);

  private:
	/// This module's global variables
	std::unordered_map<std::string, Value *> m_globals;
};
