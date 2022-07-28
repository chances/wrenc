//
// Created by znix on 10/07/2022.
//

#include "Scope.h"

ScopeStack::ScopeStack() {
	m_frames.push_back(std::make_unique<ScopeFrame>());
	m_top = m_frames.back().get();
}
ScopeStack::~ScopeStack() {}

LocalVariable *ScopeStack::Lookup(const std::string &name) {
	ScopeFrame *frame = m_top;
	while (frame) {
		auto local = frame->locals.find(name);
		if (local != frame->locals.end())
			return local->second;
		frame = frame->parent;
	}
	return nullptr;
}

bool ScopeStack::Add(LocalVariable *var) {
	// Can't add locals without a stack frame
	if (!m_top) {
		abort();
	}

	auto &locals = m_top->locals;
	if (locals.contains(var->Name()))
		return false;
	locals[var->Name()] = var;
	return true;
}

int ScopeStack::VariableCount() {
	int count = 0;
	const ScopeFrame *frame = m_top;
	while (frame) {
		count += frame->locals.size();
		frame = frame->parent;
	}
	return count;
}

void ScopeStack::PopFrame() {
	m_frames.pop_back();
	m_top = m_frames.back().get();
}

void ScopeStack::PushFrame() {
	m_frames.push_back(std::make_unique<ScopeFrame>());
	m_frames.back()->parent = m_top;
	m_top = m_frames.back().get();
}

std::vector<LocalVariable *> ScopeStack::GetFramesSince(int since) {
	std::vector<LocalVariable *> variables;
	for (int i = since; i < m_frames.size(); i++) {
		for (const auto &entry : m_frames.at(i)->locals) {
			variables.push_back(entry.second);
		}
	}
	return variables;
}

int ScopeStack::GetTopFrame() { return m_frames.size() - 1; }

std::string LocalVariable::Name() const { return name; }

VarDecl::ScopeType LocalVariable::Scope() const { return SCOPE_LOCAL; }

void LocalVariable::Accept(IRVisitor *visitor) { visitor->VisitLocalVariable(this); }

void UpvalueVariable::Accept(IRVisitor *visitor) { visitor->VisitUpvalueVariable(this); }
std::string UpvalueVariable::Name() const { return parent->Name() + "_UPVALUE"; }
VarDecl::ScopeType UpvalueVariable::Scope() const { return SCOPE_UPVALUE; }
