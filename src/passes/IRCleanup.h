//
// Created by znix on 20/07/22.
//

#pragma once

#include "IRNode.h"

#include <unordered_map>

/// Do some basic single-pass IR simplifications, namely flattening nested blocks.
class IRCleanup : private IRVisitor {
  public:
	void Process(IRNode *root);

  private:
	/// Get the nth parent node. [index]=0 means the direct parent of the current node, 1 means the
	/// parent's parent and so on.
	IRNode *GetParent(int index = 0);

	void VisitReadOnly(IRNode *node) override;
	void VisitBlock(StmtBlock *node) override;
	void VisitStmtLabel(StmtLabel *node) override;
	void VisitStmtJump(StmtJump *node) override;

	/// This function cleans up a block. If [recurse] is true it visits all the block's children so it
	/// behaves like a normal visitor function, however if [recurse] is true it does not - this is useful
	/// for cleaning up a single block after modifying it.
	void VisitBlock(StmtBlock *node, bool recurse);

	struct LabelInfo {
		StmtBlock *parent = nullptr;
		bool used = false;
	};

	std::unordered_map<StmtLabel *, LabelInfo> m_labelData;

	// The stack of the nodes leading upto this point, including the current one
	std::vector<IRNode *> m_parents;
};
