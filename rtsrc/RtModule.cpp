//
// Created by znix on 19/12/22.
//

#include "RtModule.h"

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
}

Value *RtModule::GetOrNull(const std::string &varName) {
	auto iter = m_globals.find(varName);
	if (iter == m_globals.end()) {
		return nullptr;
	}
	return iter->second;
}
