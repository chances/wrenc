//
// Created by znix on 19/12/22.
//

#pragma once

#include "common.h"

#include <memory>
#include <string>
#include <unordered_map>

class StackMapDescription;

/// Represents, and stores data about, a module at runtime
class RtModule {
  public:
	RtModule(void *globalsTable);
	~RtModule();

	/// Look up a global by varName, returning null if one isn't found
	Value *GetOrNull(const std::string &varName);

	StackMapDescription *GetStackMap();

	std::string moduleName;

  private:
	/// This module's global variables
	std::unordered_map<std::string, Value *> m_globals;

	/// The module's stack map, if available
	std::unique_ptr<StackMapDescription> m_stackMap;
};
