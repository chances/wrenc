//
// Created by znix on 10/07/2022.
//

#include "Scope.h"

LocalVariable *ScopeStack::Lookup(const std::string &name) { return nullptr; }

bool ScopeStack::Add(LocalVariable *var) { return false; }

int ScopeStack::VariableCount() {
	int count = 0;
	const ScopeFrame *frame = m_top.get();
	while (frame) {
		count += frame->locals.size();
		frame = frame->parent;
	}
	return count;
}

std::string LocalVariable::Name() const { return name; }

VarDecl::ScopeType LocalVariable::Scope() const { return SCOPE_LOCAL; }

void LocalVariable::Accept(IRVisitor *visitor) { visitor->VisitLocalVariable(this); }
