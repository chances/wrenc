//
// Created by znix on 10/07/2022.
//

#include "Module.h"
#include "ClassInfo.h"
#include "IRNode.h"

// Put the vtable here
Module::Module() = default;
Module::Module(std::optional<std::string> name) : m_name(std::move(name)) {}
Module::~Module() = default;

IRGlobalDecl *Module::AddVariable(const std::string &name) {
	auto iter = m_globals.find(name);
	if (iter != m_globals.end())
		return nullptr; // Return null if the global already exists

	IRGlobalDecl *ptr = new IRGlobalDecl();
	ptr->name = name;
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

const std::vector<IRFn *> &Module::GetFunctions() const { return m_functions; }

const IRFn *Module::GetMainFunction() const { return m_functions.front(); }

std::vector<IRFn *> Module::GetClosures() const {
	std::vector<IRFn *> result;
	for (IRFn *fn : m_functions) {
		// Anything that doesn't have an enclosing class, and is not the top-level function, is a closure
		if (fn->enclosingClass || fn == GetMainFunction())
			continue;

		result.push_back(fn);
	}
	return result;
}

const std::vector<IRClass *> &Module::GetClasses() const { return m_classes; }

void Module::AddNode(IRNode *node) {
	IRFn *func = dynamic_cast<IRFn *>(node);
	if (func) {
		m_functions.push_back(func);
		return;
	}

	IRClass *cls = dynamic_cast<IRClass *>(node);
	if (cls) {
		m_classes.push_back(cls);
		return;
	}

	// TODO error when an unsupported node is added
}
