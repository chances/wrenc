//
// Created by znix on 20/07/22.
//

#pragma once

#include "ArenaAllocator.h"
#include "IBackend.h"
#include "IRNode.h"
#include "Module.h"

#include <fmt/core.h>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

class QbeBackend : public IBackend {
  public:
	QbeBackend();
	~QbeBackend();

	CompilationResult Generate(Module *mod, const CompilationOptions *options);

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

	struct UpvaluePackDef {
		// All the variables bound to upvalues in the relevant closure
		std::vector<UpvalueVariable *> variables;

		// The positions of the variables in the upvalue pack, the inverse of variables
		std::unordered_map<UpvalueVariable *, int> variableIds;

		// The position of the variables in the variable stack storage of the parent function. This is used to generate
		// the data tables for the closures, so it can point the entries in the upvalue pack to the stack of the parent
		// function.
		std::unordered_map<UpvalueVariable *, int> valuesOnParentStack;
	};

	// Private, so declaring inline is fine
	template <typename... Args> void Print(fmt::format_string<Args...> fmtStr, Args &&...args);

	VLocal *LookupVariable(LocalVariable *decl);
	std::string GetLabelName(StmtLabel *label);
	std::string GetClosureName(IRFn *func);
	std::string MangleGlobalName(IRGlobalDecl *var);
	std::string MangleUniqueName(const std::string &name, bool excludeIdentical);
	std::string MangleRawName(const std::string &str, bool permitAmbiguity);

	Snippet *HandleUnimplemented(IRNode *node);

	VLocal *AddTemporary(std::string debugName);
	Snippet *LoadVariable(VarDecl *var);
	Snippet *StoreVariable(VarDecl *var, VLocal *input);

	void GenerateInitFunction(const std::string &moduleName, Module *mod);
	void GenerateGetGlobalFunc(const std::string &moduleName, Module *mod);

	// Create a string constant if it doesn't already exist, and return it's name, ready for use in QBE IR.
	std::string GetStringPtr(const std::string &value);
	std::string GetStringObjPtr(const std::string &value);
	static std::string EscapeString(std::string value);

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
	Snippet *VisitStmtEvalAndIgnore(StmtEvalAndIgnore *node);
	Snippet *VisitBlock(StmtBlock *node);
	Snippet *VisitStmtLabel(StmtLabel *node);
	Snippet *VisitStmtJump(StmtJump *node);
	Snippet *VisitStmtReturn(StmtReturn *node);
	Snippet *VisitStmtLoadModule(StmtLoadModule *node);
	Snippet *VisitStmtRelocateUpvalues(StmtRelocateUpvalues *node);

	Snippet *VisitExprConst(ExprConst *node);
	Snippet *VisitExprLoad(ExprLoad *node);
	Snippet *VisitExprFieldLoad(ExprFieldLoad *node);
	Snippet *VisitExprFuncCall(ExprFuncCall *node);
	Snippet *VisitExprClosure(ExprClosure *node);
	Snippet *VisitExprLoadReceiver(ExprLoadReceiver *node);
	Snippet *VisitExprRunStatements(ExprRunStatements *node);
	Snippet *VisitExprAllocateInstanceMemory(ExprAllocateInstanceMemory *node);
	Snippet *VisitExprSystemVar(ExprSystemVar *node);
	Snippet *VisitExprGetClassVar(ExprGetClassVar *node);

	bool m_inFunction = false;
	int m_exprIndentation = 0;
	std::stringstream m_output;
	std::unordered_map<LocalVariable *, std::unique_ptr<VLocal>> m_locals;
	std::vector<std::unique_ptr<VLocal>> m_temporaries;
	UpvaluePackDef *m_currentFnUpvaluePack = nullptr;
	std::unordered_map<VarDecl *, int> m_stackVariables; // Local variables that are on the stack, and their positions
	std::unordered_map<IRFn *, std::unique_ptr<UpvaluePackDef>> m_functionUpvaluePacks;

	// Stack index of linked list of each type of closure that's using upvalues.
	std::unordered_map<IRFn *, int> m_closureFnChain;

	// All the function signatures we've used, so we can put them in an array for debug messages
	std::unordered_set<std::string> m_signatures;

	// The name each function is defined as, used when generating the class data block
	std::unordered_map<IRFn *, std::string> m_functionNames;

	// String constants
	// Keys are the string literals, values are the associated symbol names
	std::unordered_map<std::string, std::string> m_strings;    // Raw C strings
	std::unordered_map<std::string, std::string> m_stringObjs; // Wren ObjString objects

	// Mapping of names that [MangleUniqueName] has encoded
	std::unordered_map<std::string, std::string> m_uniqueNames;
	std::unordered_set<std::string> m_uniqueNamesInv; // Inverse of m_uniqueNames

	// Place to allocate all our snippets
	ArenaAllocator m_alloc;

	static constexpr char PTR_TYPE = 'l';
};
