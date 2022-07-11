//
// Created by znix on 10/07/2022.
//

#include "Module.h"
#include "IRNode.h"

IRGlobalDecl *Module::AddVariable(const std::string &name) {
	auto iter = m_globals.find(name);
	if (iter == m_globals.end())
		return nullptr;

	IRGlobalDecl *ptr = new IRGlobalDecl();
	m_globals[name] = std::unique_ptr<IRGlobalDecl>(ptr);

	return ptr;
}
