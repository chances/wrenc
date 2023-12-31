//
// Created by znix on 10/07/2022.
//

#include "Scope.h"
#include "VarType.h"

ScopeStack::ScopeStack() {}
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

	// Add it to the create variables block, in case it's later used as an upvalue
	if (m_top->upvalueContainer) {
		m_top->upvalueContainer->variables.push_back(var);
		var->beginUpvalues = m_top->upvalueContainer;
	}

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

void ScopeStack::PushFrame(StmtBeginUpvalues *upvalues) {
	m_frames.push_back(std::make_unique<ScopeFrame>());
	m_frames.back()->parent = m_top;
	m_frames.back()->upvalueContainer = upvalues;
	m_top = m_frames.back().get();
}

std::vector<ScopeFrame *> ScopeStack::GetFramesSince(int since) {
	std::vector<ScopeFrame *> frames;
	for (int i = since; i < m_frames.size(); i++) {
		frames.push_back(m_frames.at(i).get());
	}
	return frames;
}

int ScopeStack::GetTopFrame() { return m_frames.size() - 1; }

std::string LocalVariable::Name() const { return name; }

VarDecl::ScopeType LocalVariable::Scope() const { return SCOPE_LOCAL; }

void LocalVariable::Accept(IRVisitor *visitor) { visitor->VisitLocalVariable(this); }

std::string SSAVariable::Name() const { return name; }
VarDecl::ScopeType SSAVariable::Scope() const { return VarDecl::SCOPE_LOCAL; }
void SSAVariable::Accept(IRVisitor *visitor) { visitor->VisitSSAVariable(this); }

void UpvalueVariable::Accept(IRVisitor *visitor) { visitor->VisitUpvalueVariable(this); }
std::string UpvalueVariable::Name() const { return parent->Name() + "_UPVALUE"; }
VarDecl::ScopeType UpvalueVariable::Scope() const { return SCOPE_UPVALUE; }

LocalVariable *UpvalueVariable::GetFinalTarget() const {
	VarDecl *var = parent;
	while (true) {
		if (var->Scope() == SCOPE_LOCAL) {
			return dynamic_cast<LocalVariable *>(var);
		}

		UpvalueVariable *upvalue = dynamic_cast<UpvalueVariable *>(var);
		var = upvalue->parent;
	}
}
