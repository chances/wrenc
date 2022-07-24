//
// Created by znix on 10/07/2022.
//

#include "SymbolTable.h"

// Make sure the vtable ends up in this compilation unit
SymbolTable::~SymbolTable() = default;

FieldVariable *SymbolTable::Ensure(const std::string &name) {
	auto iter = byName.find(name);
	if (iter != byName.end())
		return iter->second;

	// Can't use make_unique since the constructor is private
	FieldVariable *var = new FieldVariable();
	var->m_fieldId = fields.size();
	var->m_name = name;
	fields.push_back(std::unique_ptr<FieldVariable>(var));
	byName[name] = var;
	return var;
}

FieldVariable::FieldVariable() = default;
FieldVariable::~FieldVariable() = default;
