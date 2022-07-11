//
// Created by znix on 10/07/2022.
//

#include "Scope.h"

LocalVariable *ScopeStack::Lookup(const std::string &name) { return nullptr; }

bool ScopeStack::Add(LocalVariable *var) { return false; }

std::string LocalVariable::Name() const { return name; }

VarDecl::ScopeType LocalVariable::Scope() const { return SCOPE_LOCAL; }
