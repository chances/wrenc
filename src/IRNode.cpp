//
// Created by znix on 10/07/2022.
//

#include "IRNode.h"

#include "ArenaAllocator.h"
#include "Scope.h"

// Ugly hack to get the allocator out of a compiler
ArenaAllocator *getCompilerAlloc(Compiler *compiler);

VarDecl::~VarDecl() = default;
IRNode::~IRNode() = default;

std::string IRGlobalDecl::Name() const { return name; }
VarDecl::ScopeType IRGlobalDecl::Scope() const { return SCOPE_MODULE; }

void StmtBlock::Add(IRStmt *stmt) {
	if (stmt)
		statements.push_back(stmt);
}
void StmtBlock::Add(Compiler *forAlloc, IRExpr *expr) {
	if (!expr)
		return;

	ArenaAllocator *alloc = getCompilerAlloc(forAlloc);
	Add(alloc->New<StmtEvalAndIgnore>(expr));
}

ExprConst::ExprConst() {}
ExprConst::ExprConst(CcValue value) : value(value) {}

std::string StmtUpvalueImport::Name() const { return parent->Name() + "/UPVALUE"; }
VarDecl::ScopeType StmtUpvalueImport::Scope() const { return SCOPE_UPVALUE; }

IRVisitor::~IRVisitor() {}

// Top-level-node accept functions
void IRFn::Accept(IRVisitor *visitor) { visitor->VisitFn(this); }
void IRClass::Accept(IRVisitor *visitor) { visitor->VisitClass(this); }
void IRGlobalDecl::Accept(IRVisitor *visitor) { visitor->VisitGlobalDecl(this); }
void IRImport::Accept(IRVisitor *visitor) { visitor->VisitImport(this); }

// Statements
void StmtAssign::Accept(IRVisitor *visitor) { visitor->VisitStmtAssign(this); }
void StmtFieldAssign::Accept(IRVisitor *visitor) { visitor->VisitStmtFieldAssign(this); }
void StmtUpvalueImport::Accept(IRVisitor *visitor) { visitor->VisitStmtUpvalue(this); }
void StmtEvalAndIgnore::Accept(IRVisitor *visitor) { visitor->VisitStmtEvalAndIgnore(this); }
void StmtBlock::Accept(IRVisitor *visitor) { visitor->VisitBlock(this); } // Use Block not StmtBlock since it's special
void StmtLabel::Accept(IRVisitor *visitor) { visitor->VisitStmtLabel(this); }
void StmtJump::Accept(IRVisitor *visitor) { visitor->VisitStmtJump(this); }
void StmtReturn::Accept(IRVisitor *visitor) { visitor->VisitStmtReturn(this); }
void StmtLoadModule::Accept(IRVisitor *visitor) { visitor->VisitStmtLoadModule(this); }

// Expressions
void ExprConst::Accept(IRVisitor *visitor) { visitor->VisitExprConst(this); }
void ExprLoad::Accept(IRVisitor *visitor) { visitor->VisitExprLoad(this); }
void ExprFieldLoad::Accept(IRVisitor *visitor) { visitor->VisitExprFieldLoad(this); }
void ExprFuncCall::Accept(IRVisitor *visitor) { visitor->VisitExprFuncCall(this); }
void ExprClosure::Accept(IRVisitor *visitor) { visitor->VisitExprClosure(this); }
void ExprLoadReceiver::Accept(IRVisitor *visitor) { visitor->VisitExprLoadReceiver(this); }
void ExprRunStatements::Accept(IRVisitor *visitor) { visitor->VisitExprRunStatements(this); }
void ExprLogicalNot::Accept(IRVisitor *visitor) { visitor->VisitExprLogicalNot(this); }
void ExprAllocateInstanceMemory::Accept(IRVisitor *visitor) { visitor->VisitExprAllocateInstanceMemory(this); }

// IRVisitor

void IRVisitor::Visit(IRNode *node) {
	if (node)
		node->Accept(this);
}
void IRVisitor::VisitVar(VarDecl *var) {
	if (var)
		var->Accept(this);
}

void IRVisitor::VisitFn(IRFn *node) {
	for (LocalVariable *var : node->locals)
		var->Accept(this);

	for (const auto &entry : node->upvalues) {
		VisitVar(entry.first);
		// Ignore entry.second because upvalue imports are scattered around the tree
	}

	for (StmtUpvalueImport *import : node->unInsertedImports)
		Visit(import);

	Visit(node->body);
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
	Visit(node->object);
	Visit(node->value);
}
void IRVisitor::VisitStmtUpvalue(StmtUpvalueImport *node) { VisitVar(node->parent); }
void IRVisitor::VisitStmtEvalAndIgnore(StmtEvalAndIgnore *node) { Visit(node->expr); }
void IRVisitor::VisitBlock(StmtBlock *node) {
	for (IRStmt *stmt : node->statements)
		Visit(stmt);
}
void IRVisitor::VisitStmtLabel(StmtLabel *node) {}
void IRVisitor::VisitStmtJump(StmtJump *node) { Visit(node->condition); }
void IRVisitor::VisitStmtReturn(StmtReturn *node) { Visit(node->value); }
void IRVisitor::VisitStmtLoadModule(StmtLoadModule *node) {
	for (const StmtLoadModule::VarImport &import : node->variables)
		VisitVar(import.bindTo);
}
void IRVisitor::VisitExprConst(ExprConst *node) {}
void IRVisitor::VisitExprLoad(ExprLoad *node) { VisitVar(node->var); }
void IRVisitor::VisitExprFieldLoad(ExprFieldLoad *node) { Visit(node->object); }
void IRVisitor::VisitExprFuncCall(ExprFuncCall *node) {
	Visit(node->receiver);
	for (IRExpr *expr : node->args)
		Visit(expr);
}
void IRVisitor::VisitExprClosure(ExprClosure *node) {
	// Closures are only generated for anonymous functions, which are only accessible through these nodes, so
	// it's fine to visit them.
	Visit(node->func);
}
void IRVisitor::VisitExprLoadReceiver(ExprLoadReceiver *node) {}
void IRVisitor::VisitExprRunStatements(ExprRunStatements *node) {
	VisitVar(node->temporary);
	Visit(node->statement);
}
void IRVisitor::VisitExprLogicalNot(ExprLogicalNot *node) { Visit(node->input); }
void IRVisitor::VisitExprAllocateInstanceMemory(ExprAllocateInstanceMemory *node) {
	// Our IRClass nodes are already in the tree, don't visit them
}
