//
// Created by znix on 10/07/2022.
//

#include "SymbolTable.h"

// Make sure the vtable ends up in this compilation unit
SymbolTable::~SymbolTable() = default;

FieldVariable* SymbolTable::Ensure(const std::string &name) {
	abort(); // TODO implement
	return 0;
}

FieldVariable::FieldVariable() = default;
FieldVariable::~FieldVariable() = default;
