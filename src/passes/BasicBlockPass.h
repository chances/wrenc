//
// Created by znix on 28/12/22.
//

#pragma once

#include "IRNode.h"

class ArenaAllocator;

class BasicBlockPass {
  public:
	BasicBlockPass(ArenaAllocator *allocator);

	void Process(IRFn *fn);

  private:
	ArenaAllocator *m_allocator = nullptr;

	StmtBlock *CreateBasicBlock();

	/// Rather than filtering nodes out of the outer block's contents list, add them to a new one then move
	/// that into the outer block when we're done, since the original and final lists are distinct so it's
	/// a waste of time trying to modify it.
	std::vector<IRStmt *> m_newContents;
};
