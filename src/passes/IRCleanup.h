//
// Created by znix on 20/07/22.
//

#pragma once

#include "IRNode.h"

#include <unordered_map>

/// Do some basic single-pass IR simplifications, such as flattening nested blocks and ExprRunStatements.
class IRCleanup : private IRVisitor {
  public:
	void Process(IRNode *root);

  private:
	/// Get the nth parent node. [index]=0 means the direct parent of the current node, 1 means the
	/// parent's parent and so on.
	IRNode *GetParent(int index = 0);

	void Visit(IRNode *node) override;
	void VisitBlock(StmtBlock *node) override;
	void VisitStmtLabel(StmtLabel *node) override;
	void VisitStmtJump(StmtJump *node) override;

	struct LabelInfo {
		StmtBlock *parent = nullptr;
		bool used = false;
	};

	std::unordered_map<StmtLabel *, LabelInfo> m_labelData;

	// The stack of the nodes leading upto this point, including the current one
	std::vector<IRNode *> m_parents;
};
