//
// Created by znix on 19/12/22.
//

#include "RtModule.h"

#include "ObjString.h"
#include "StackMapDescription.h"

RtModule::RtModule(void *globalsTable) {
	// Read in all the global table items and put them in our map
	struct {
		const char *name;
		Value *global;
	} *items;
	items = (decltype(items))globalsTable;

	while (items->name) {
		m_globals[items->name] = items->global;
		items++;
	}

	// The name is inserted as a special value
	const char *rawName = (const char *)GetOrNull("<INTERNAL>::module_name");
	if (rawName == nullptr) {
		moduleName = "<unnamed module>";
	} else {
		moduleName = rawName;
	}

	// Parse the stackmap, if present
	const void *stackMapPtr = GetOrNull("<INTERNAL>::stack_map");
	if (stackMapPtr) {
		m_stackMap = std::make_unique<StackMapDescription>(stackMapPtr);
	}
}

RtModule::~RtModule() {}

Value *RtModule::GetOrNull(const std::string &varName) {
	auto iter = m_globals.find(varName);
	if (iter == m_globals.end()) {
		return nullptr;
	}
	return iter->second;
}

StackMapDescription *RtModule::GetStackMap() { return m_stackMap.get(); }

void RtModule::MarkModuleGCValues(GCMarkOps *ops) {
	// Mark all the string constants
	ops->ReportObjects(ops, (Obj **)m_stringConstants.data(), m_stringConstants.size());

	// Mark all the global variables
	for (const auto &[key, ptr] : m_globals) {
		// Skip the various bits and pieces we store in this table, like
		// strings and function pointers.
		if (key.starts_with("<INTERNAL>::"))
			continue;

		ops->ReportValue(ops, *ptr);
	}
}

ObjString *RtModule::CreateStringLiteral(std::string &&str) {
	ObjString *obj = ObjString::New(str);
	m_stringConstants.push_back(obj);
	return obj;
}
