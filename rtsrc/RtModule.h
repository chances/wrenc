//
// Created by znix on 19/12/22.
//

#pragma once

#include "Obj.h"
#include "common/common.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class StackMapDescription;

/// Represents, and stores data about, a module at runtime
class RtModule {
  public:
	RtModule(void *globalsTable);
	~RtModule();

	/// Look up a global by varName, returning null if one isn't found
	Value *GetOrNull(const std::string &varName);

	StackMapDescription *GetStackMap();

	/// Mark the values held as roots by this module, for example global
	/// variables and string constants.
	/// This is very similar to MarkGCValues from Obj.
	void MarkModuleGCValues(GCMarkOps *ops);

	/// Create a string literal, and register it as a GC root.
	/// WARNING: This should *ONLY* be called from GenEntry!
	ObjString *CreateStringLiteral(std::string &&str);

	std::string moduleName;

  private:
	/// This module's global variables
	std::unordered_map<std::string, Value *> m_globals;

	/// The module's stack map, if available
	std::unique_ptr<StackMapDescription> m_stackMap;

	/// The string constants defined in this module, used for GC marking.
	std::vector<ObjString *> m_stringConstants;
};
