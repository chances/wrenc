//
// Created by znix on 10/07/2022.
//

#include "IRNode.h"

#include "ArenaAllocator.h"
#include "ClassInfo.h"
#include "CompContext.h"
#include "Scope.h"

#include <fmt/format.h>

IRClass::~IRClass() = default;
VarDecl::~VarDecl() = default;
BackendNodeData::~BackendNodeData() = default;
IRNode::~IRNode() = default;

std::string IRGlobalDecl::Name() const { return name; }
VarDecl::ScopeType IRGlobalDecl::Scope() const { return SCOPE_MODULE; }

bool IRStmt::IsUnconditionalBranch() { return false; }

void StmtBlock::Add(IRStmt *stmt) {
	if (stmt)
		statements.push_back(stmt);
}

StmtAssign::StmtAssign(VarDecl *var, IRExpr *expr) : var(var), expr(expr) {
	SSAVariable *ssa = dynamic_cast<SSAVariable *>(var);
	if (ssa) {
		if (ssa->assignment) {
			fmt::print(stderr, "Cannot create an StmtAssign on already-assigned SSAVariable '{}'\n", ssa->name.c_str());
			abort();
		}
		ssa->assignment = this;
	}
}

ExprConst::ExprConst() {}
ExprConst::ExprConst(CcValue value) : value(value) {}

bool IRExpr::IsPure() const { return false; }
bool ExprConst::IsPure() const { return true; }
bool ExprLoad::IsPure() const { return true; } // Loading an already-defined variable is never an error.
bool ExprFieldLoad::IsPure() const { return true; }
bool ExprPhi::IsPure() const { return true; }
bool ExprLoadReceiver::IsPure() const { return true; }
bool ExprSystemVar::IsPure() const { return true; }
bool ExprGetClassVar::IsPure() const { return true; }

ExprSystemVar::ExprSystemVar(std::string name) : name(name) {
	if (!SYSTEM_VAR_NAMES.contains(name)) {
		fmt::print(stderr, "Illegal system variable name '{}'\n", name);
		abort();
	}
}

bool StmtReturn::IsUnconditionalBranch() { return true; }

bool StmtJump::IsUnconditionalBranch() { return condition == nullptr; }

static std::unordered_map<std::string, int> buildSysVarNames(bool cppOnly) {
	std::vector<const char *> names = {
	    "Bool",
	    "Class",
	    "Fiber",
	    "Fn",
	    "List",
	    "Map",
	    "Null",
	    "Num",
	    "Object",
	    "Range",
	    "String",
	    "System",
	};

	// "soft" system variables that are basically normal classes, but
	// in the core module. Unlike the above, they don't have a part
	// of them written in C++.
	std::vector<const char *> softNames = {
	    "Sequence",
	    "MapSequence",
	    "SkipSequence",
	    "TakeSequence",
	    "WhereSequence",
	    "StringByteSequence",
	    "StringCodePointSequence",
	    "MapEntry",
	    "MapKeySequence",
	    "MapValueSequence",
	    "ClassAttributes",
	};

	if (!cppOnly) {
		// Assign IDs for all the names
		names.insert(names.end(), softNames.begin(), softNames.end());
	}

	// Note the IDs will match between the CPP-only and full maps, since
	// the CPP names come first.
	std::unordered_map<std::string, int> result;
	for (const char *value : names) {
		result[value] = result.size();
	}
	return result;
}
const std::unordered_map<std::string, int> ExprSystemVar::SYSTEM_VAR_NAMES = buildSysVarNames(false);
const std::unordered_map<std::string, int> ExprSystemVar::CPP_SYSTEM_VAR_NAMES = buildSysVarNames(true);

const std::unordered_map<std::string, int> &ExprSystemVar::GetSystemVarNames(bool cpp) {
	return cpp ? CPP_SYSTEM_VAR_NAMES : SYSTEM_VAR_NAMES;
}

IRVisitor::~IRVisitor() {}

// Top-level-node accept functions
void IRFn::Accept(IRVisitor *visitor) { visitor->VisitFn(this); }
void IRClass::Accept(IRVisitor *visitor) { visitor->VisitClass(this); }
void IRGlobalDecl::Accept(IRVisitor *visitor) { visitor->VisitGlobalDecl(this); }
void IRImport::Accept(IRVisitor *visitor) { visitor->VisitImport(this); }

// Statements
void StmtAssign::Accept(IRVisitor *visitor) { visitor->VisitStmtAssign(this); }
void StmtFieldAssign::Accept(IRVisitor *visitor) { visitor->VisitStmtFieldAssign(this); }
void StmtEvalAndIgnore::Accept(IRVisitor *visitor) { visitor->VisitStmtEvalAndIgnore(this); }
void StmtBlock::Accept(IRVisitor *visitor) { visitor->VisitBlock(this); } // Use Block not StmtBlock since it's special
void StmtLabel::Accept(IRVisitor *visitor) { visitor->VisitStmtLabel(this); }
void StmtJump::Accept(IRVisitor *visitor) { visitor->VisitStmtJump(this); }
void StmtReturn::Accept(IRVisitor *visitor) { visitor->VisitStmtReturn(this); }
void StmtLoadModule::Accept(IRVisitor *visitor) { visitor->VisitStmtLoadModule(this); }
void StmtBeginUpvalues::Accept(IRVisitor *visitor) { visitor->VisitStmtBeginUpvalues(this); }
void StmtRelocateUpvalues::Accept(IRVisitor *visitor) { visitor->VisitStmtRelocateUpvalues(this); }
void StmtDefineClass::Accept(IRVisitor *visitor) { visitor->VisitStmtDefineClass(this); }

// Expressions
void ExprConst::Accept(IRVisitor *visitor) { visitor->VisitExprConst(this); }
void ExprLoad::Accept(IRVisitor *visitor) { visitor->VisitExprLoad(this); }
void ExprFieldLoad::Accept(IRVisitor *visitor) { visitor->VisitExprFieldLoad(this); }
void ExprPhi::Accept(IRVisitor *visitor) { visitor->VisitExprPhi(this); }
void ExprFuncCall::Accept(IRVisitor *visitor) { visitor->VisitExprFuncCall(this); }
void ExprClosure::Accept(IRVisitor *visitor) { visitor->VisitExprClosure(this); }
void ExprLoadReceiver::Accept(IRVisitor *visitor) { visitor->VisitExprLoadReceiver(this); }
void ExprRunStatements::Accept(IRVisitor *visitor) { visitor->VisitExprRunStatements(this); }
void ExprAllocateInstanceMemory::Accept(IRVisitor *visitor) { visitor->VisitExprAllocateInstanceMemory(this); }
void ExprSystemVar::Accept(IRVisitor *visitor) { visitor->VisitExprSystemVar(this); }
void ExprGetClassVar::Accept(IRVisitor *visitor) { visitor->VisitExprGetClassVar(this); }

// IRVisitor

void IRVisitor::VisitReadOnly(IRNode *node) {
	if (node)
		node->Accept(this);
}
void IRVisitor::Visit(IRExpr *&node) { VisitReadOnly(node); }
void IRVisitor::Visit(IRStmt *&node) { VisitReadOnly(node); }
void IRVisitor::VisitVar(VarDecl *var) {
	if (var)
		var->Accept(this);
}

void IRVisitor::VisitFn(IRFn *node) {
	for (LocalVariable *var : node->locals)
		var->Accept(this);

	for (const auto &entry : node->upvalues) {
		// Don't visit entry.first since that's a variable from a different function
		VisitVar(entry.second);
	}

	IRStmt *body = node->body;
	Visit(body);
	node->body = dynamic_cast<StmtBlock *>(body);
	if (!node->body) {
		fprintf(stderr, "Cannot convert function body to non-block '%s' in function '%s'!\n", typeid(body).name(),
		    node->debugName.c_str());
		abort();
	}
}
void IRVisitor::VisitClass(IRClass *node) {}
void IRVisitor::VisitGlobalDecl(IRGlobalDecl *node) {}
void IRVisitor::VisitImport(IRImport *node) {}
void IRVisitor::VisitStmtAssign(StmtAssign *node) {
	VisitVar(node->var);
	Visit(node->expr);
}
void IRVisitor::VisitStmtFieldAssign(StmtFieldAssign *node) {
	// node->var isn't a VarDecl
	Visit(node->value);
}
void IRVisitor::VisitStmtEvalAndIgnore(StmtEvalAndIgnore *node) { Visit(node->expr); }
void IRVisitor::VisitBlock(StmtBlock *node) {
	for (IRStmt *&stmt : node->statements)
		Visit(stmt);
}
void IRVisitor::VisitStmtLabel(StmtLabel *node) {}
void IRVisitor::VisitStmtJump(StmtJump *node) { Visit(node->condition); }
void IRVisitor::VisitStmtReturn(StmtReturn *node) { Visit(node->value); }
void IRVisitor::VisitStmtLoadModule(StmtLoadModule *node) {
	for (const StmtLoadModule::VarImport &import : node->variables)
		VisitVar(import.bindTo);
}
void IRVisitor::VisitStmtBeginUpvalues(StmtBeginUpvalues *node) {}
void IRVisitor::VisitStmtRelocateUpvalues(StmtRelocateUpvalues *node) {}
void IRVisitor::VisitStmtDefineClass(StmtDefineClass *node) {
	// Don't visit the class, that's already part of the module
	VisitVar(node->outputVariable);
}
void IRVisitor::VisitExprConst(ExprConst *node) {}
void IRVisitor::VisitExprLoad(ExprLoad *node) { VisitVar(node->var); }
void IRVisitor::VisitExprFieldLoad(ExprFieldLoad *node) {}
void IRVisitor::VisitExprPhi(ExprPhi *node) {
	for (SSAVariable *var : node->inputs)
		VisitVar(var);
}
void IRVisitor::VisitExprFuncCall(ExprFuncCall *node) {
	Visit(node->receiver);
	for (IRExpr *&expr : node->args)
		Visit(expr);
}
void IRVisitor::VisitExprClosure(ExprClosure *node) {
	// The functions closures bind to are added to the module directly, we'd see them twice if we visited it here
}
void IRVisitor::VisitExprLoadReceiver(ExprLoadReceiver *node) {}
void IRVisitor::VisitExprRunStatements(ExprRunStatements *node) {
	VisitVar(node->temporary);
	Visit(node->statement);
}
void IRVisitor::VisitExprAllocateInstanceMemory(ExprAllocateInstanceMemory *node) {
	// Our IRClass nodes are already in the tree, don't visit them
	for (SSAVariable *var : node->foreignParameters)
		VisitVar(var);
}
void IRVisitor::VisitExprSystemVar(ExprSystemVar *node) {}
void IRVisitor::VisitExprGetClassVar(ExprGetClassVar *node) {}

void IRVisitor::VisitLocalVariable(LocalVariable *var) {}
void IRVisitor::VisitSSAVariable(SSAVariable *var) {}
void IRVisitor::VisitUpvalueVariable(UpvalueVariable *var) {}
