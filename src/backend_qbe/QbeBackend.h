//
// Created by znix on 20/07/22.
//

#pragma once

#include "ArenaAllocator.h"
#include "IRNode.h"
#include "Module.h"

#include <fmt/core.h>
#include <memory>
#include <sstream>

class QbeBackend {
  public:
	QbeBackend();
	~QbeBackend();

	void Generate(Module *module);

  private:
	/// Information about a local variable
	struct VLocal {
		std::string name; // Name in the QBE IR
	};

	struct Snippet {
		std::vector<std::string> lines;

		// Private, so declaring inline is fine
		template <typename... Args> void Add(fmt::format_string<Args...> fmtStr, Args &&...args);

		// Add a snippet to this snippet, and return that snippet's result
		VLocal *Add(Snippet *other);

		// If this snippet is for an expression, this is the register the result is placed in
		VLocal *result = nullptr;
	};

	// Private, so declaring inline is fine
	template <typename... Args> void Print(fmt::format_string<Args...> fmtStr, Args &&...args);

	VLocal *LookupVariable(LocalVariable *decl);
	std::string MangleGlobalName(IRGlobalDecl *var);
	std::string MangleUniqueName(const std::string &name);
	std::string MangleRawName(const std::string &str, bool permitAmbiguity);

	Snippet *HandleUnimplemented(IRNode *node);

	VLocal *AddTemporary(std::string debugName);

	// Create a string constant if it doesn't already exist, and return it's name, ready for use in QBE IR.
	std::string GetStringPtr(const std::string &value);
	std::string GetStringObjPtr(const std::string &value);

	Snippet *VisitExpr(IRExpr *expr);
	Snippet *VisitStmt(IRStmt *expr);

	// We're not actually an IRVisitor since we have to have special logic for every node so there's
	// no point using any of the normal visitor logic.
	void VisitFn(IRFn *node, std::optional<std::string> initFunction);
	Snippet *VisitClass(IRClass *node);
	Snippet *VisitGlobalDecl(IRGlobalDecl *node);
	Snippet *VisitImport(IRImport *node);

	Snippet *VisitStmtAssign(StmtAssign *node);
	Snippet *VisitStmtFieldAssign(StmtFieldAssign *node);
	Snippet *VisitStmtUpvalue(StmtUpvalueImport *node);
	Snippet *VisitStmtEvalAndIgnore(StmtEvalAndIgnore *node);
	Snippet *VisitBlock(StmtBlock *node);
	Snippet *VisitStmtLabel(StmtLabel *node);
	Snippet *VisitStmtJump(StmtJump *node);
	Snippet *VisitStmtReturn(StmtReturn *node);
	Snippet *VisitStmtLoadModule(StmtLoadModule *node);

	Snippet *VisitExprConst(ExprConst *node);
	Snippet *VisitExprLoad(ExprLoad *node);
	Snippet *VisitExprFieldLoad(ExprFieldLoad *node);
	Snippet *VisitExprFuncCall(ExprFuncCall *node);
	Snippet *VisitExprClosure(ExprClosure *node);
	Snippet *VisitExprLoadReceiver(ExprLoadReceiver *node);
	Snippet *VisitExprRunStatements(ExprRunStatements *node);
	Snippet *VisitExprLogicalNot(ExprLogicalNot *node);
	Snippet *VisitExprAllocateInstanceMemory(ExprAllocateInstanceMemory *node);
	Snippet *VisitExprSystemVar(ExprSystemVar *node);

	bool m_inFunction = false;
	int m_exprIndentation = 0;
	std::stringstream m_output;
	std::unordered_map<LocalVariable *, std::unique_ptr<VLocal>> m_locals;
	std::vector<std::unique_ptr<VLocal>> m_temporaries;

	// String constants
	// Keys are the string literals, values are the associated symbol names
	std::unordered_map<std::string, std::string> m_strings;    // Raw C strings
	std::unordered_map<std::string, std::string> m_stringObjs; // Wren ObjString objects

	// Mapping of names that [MangleUniqueName] has encoded
	std::unordered_map<std::string, std::string> m_uniqueNames;

	// Place to allocate all our snippets
	ArenaAllocator m_alloc;

	static constexpr char PTR_TYPE = 'l';
	void GenerateInitFunction(const std::string &initFuncName);
};
