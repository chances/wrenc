//
// Created by znix on 20/07/22.
//

#pragma once

#include "IRNode.h"

#include <unordered_map>

class ArenaAllocator;

/// Do some basic single-pass IR simplifications, namely flattening nested blocks.
class IRCleanup : private IRVisitor {
  public:
	IRCleanup(ArenaAllocator *allocator);

	void Process(IRNode *root);

  private:
	/// Get the nth parent node. [index]=0 means the direct parent of the current node, 1 means the
	/// parent's parent and so on.
	IRNode *GetParent(int index = 0);

	void VisitReadOnly(IRNode *node) override;
	void Visit(IRExpr *&node) override;
	void VisitBlock(StmtBlock *node) override;
	void VisitStmtLabel(StmtLabel *node) override;
	void VisitStmtJump(StmtJump *node) override;
	void VisitFn(IRFn *node) override;

	/// This function cleans up a block. If [recurse] is true it visits all the block's children so it
	/// behaves like a normal visitor function, however if [recurse] is true it does not - this is useful
	/// for cleaning up a single block after modifying it.
	void VisitBlock(StmtBlock *node, bool recurse);

	/// Similar to VisitExprRunStatements, but specially called so it can replace itself with something else.
	IRExpr *SubstituteExprRunStatements(ExprRunStatements *node);

	/// Similar to VisitExprFuncCall, but specially called so it can replace itself with something else.
	/// This is done so actions performed in a list are done in order, even when some are in an ExprRunStatements
	/// and thus get moved out.
	IRExpr *SubstituteExprFuncCall(ExprFuncCall *node);

	struct LabelInfo {
		StmtBlock *parent = nullptr;
		bool used = false;
	};

	struct RunStatementsTarget {
		StmtBlock *block = nullptr;
		int insertIndex = 0;
		bool inserted = false;
	};

	std::unordered_map<StmtLabel *, LabelInfo> m_labelData;

	// The stack of the nodes leading upto this point, including the current one
	std::vector<IRNode *> m_parents;
	std::vector<IRFn *> m_fnParents; // Same as m_parents but for functions

	// The current block, into which we should insert the contents of ExprRunStatements nodes
	RunStatementsTarget m_runStatementsTarget;

	ArenaAllocator *m_allocator = nullptr;
};
