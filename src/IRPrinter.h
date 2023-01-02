//
// Created by znix on 28/12/22.
//

#pragma once

#include "IRNode.h"

class IRPrinter : private IRVisitor {
  public:
	IRPrinter();
	~IRPrinter();

	std::unique_ptr<std::stringstream> Extract();

	void Process(IRNode *root);

  private:
	struct Tag {
		std::string header;
		std::string indent;
		std::vector<std::string> components;
		bool isInline = false;
	};

	void VisitReadOnly(IRNode *node) override;
	void VisitVar(VarDecl *var) override;

	void FullTag(const std::string &str);
	void StartTag(const std::string &str, bool printInline);
	void EndTag();

	void VisitExprConst(ExprConst *node) override;
	void VisitExprFuncCall(ExprFuncCall *node) override;
	void VisitStmtLabel(StmtLabel *node) override;
	void VisitStmtJump(StmtJump *node) override;
	void VisitExprSystemVar(ExprSystemVar *node) override;
	void VisitExprGetClassVar(ExprGetClassVar *node) override;
	void VisitFn(IRFn *node) override;
	void VisitStmtFieldAssign(StmtFieldAssign *node) override;
	void VisitExprFieldLoad(ExprFieldLoad *node) override;
	void VisitExprAllocateInstanceMemory(ExprAllocateInstanceMemory *node) override;
	void VisitExprClosure(ExprClosure *node) override;
	void VisitStmtBeginUpvalues(StmtBeginUpvalues *node) override;
	void VisitStmtRelocateUpvalues(StmtRelocateUpvalues *node) override;
	void VisitBlock(StmtBlock *node) override;

	std::string GetLabelId(StmtLabel *label, bool colourise);
	std::string GetBeginUpvaluesId(StmtBeginUpvalues *upvalue);

	/// Colourise (using ANSI escape codes) a given string, with a colour derived by hashing a pointer.
	static std::string Colourise(const void *ptr, const std::string &input);

	std::vector<Tag> m_tagStack;
	std::unique_ptr<std::stringstream> m_stream;

	std::unordered_map<StmtLabel *, int> m_labelIds;
	std::unordered_map<StmtBeginUpvalues *, int> m_beginUpvaluesIds;
};
