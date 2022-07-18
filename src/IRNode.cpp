//
// Created by znix on 10/07/2022.
//

#include "IRNode.h"

#include "ArenaAllocator.h"

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
