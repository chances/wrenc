//
// Created by znix on 10/07/2022.
//

#include "Module.h"
#include "ClassInfo.h"
#include "IRNode.h"

// Put the vtable here
Module::Module() = default;
Module::~Module() = default;

IRGlobalDecl *Module::AddVariable(const std::string &name) {
	auto iter = m_globals.find(name);
	if (iter != m_globals.end())
		return nullptr; // Return null if the global already exists

	IRGlobalDecl *ptr = new IRGlobalDecl();
	m_globals[name] = std::unique_ptr<IRGlobalDecl>(ptr);

	return ptr;
}

IRGlobalDecl *Module::FindVariable(const std::string &name) {
	auto iter = m_globals.find(name);
	if (iter == m_globals.end())
		return nullptr;
	return iter->second.get();
}

std::vector<IRGlobalDecl *> Module::GetGlobalVariables() {
	std::vector<IRGlobalDecl *> vars;
	vars.reserve(m_globals.size());
	for (const auto &entry : m_globals) {
		vars.push_back(entry.second.get());
	}
	return vars;
}

void Module::AddNode(IRNode *node) {}
