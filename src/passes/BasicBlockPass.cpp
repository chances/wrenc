//
// Created by znix on 28/12/22.
//

#include "BasicBlockPass.h"
#include "ArenaAllocator.h"

#include <fmt/format.h>

BasicBlockPass::BasicBlockPass(ArenaAllocator *allocator) : m_allocator(allocator) {}

void BasicBlockPass::Process(IRFn *fn) {
	StmtBlock *contents = dynamic_cast<StmtBlock *>(fn->body);

	// See the comment on m_newContents - basically, this will replace contents->statements
	m_newContents.clear();

	StmtBlock *current = CreateBasicBlock();

	// Keep track of whether the last statement was an unconditional branch of some kind - either an actual
	// unconditional jump, or a return. This is used when adding a label to check if we need a fallthrough jump.
	bool lastWasUnconditionalBranch = false;

	for (size_t i = 0; i < contents->statements.size(); i++) {
		IRStmt *statement = contents->statements.at(i);

		bool prevUnconditionalBranch = lastWasUnconditionalBranch;
		lastWasUnconditionalBranch = statement->IsUnconditionalBranch();

		if (dynamic_cast<StmtBlock *>(statement)) {
			fmt::print(stderr, "Nested blocks are not allowed on entry to the SSA pass");
			abort();
		}

		StmtLabel *label = dynamic_cast<StmtLabel *>(statement);
		if (label) {
			// Add a jump, jumping to the new block from the old block
			// (except if the last statement was an unconditional jump, in which case this would be dead code and
			// also illegal by basic block rules)
			if (!prevUnconditionalBranch) {
				StmtJump *jumpToNextBlock = m_allocator->New<StmtJump>(label, nullptr);
				jumpToNextBlock->debugInfo.synthetic = true;
				current->statements.push_back(jumpToNextBlock);
			}

			// Then put the label as the first instruction of the new block
			current = CreateBasicBlock();
			current->statements.push_back(statement);
			statement->basicBlock = current;

			continue;
		}

		current->statements.push_back(statement);
		statement->basicBlock = current;

		// Unconditional branches (which includes returns) end the current block, without a proceeding one. No
		// statements should appear until the next label.
		if (statement->IsUnconditionalBranch()) {
			current = nullptr;
			continue;
		}

		// Handle conditional jumps - unconditional jumps have already been handled.
		StmtJump *jump = dynamic_cast<StmtJump *>(statement);
		if (jump) {
			// Insert a block boundary at the end of this jump. This requires a fallthrough jump/label pair.
			// This does mean the current block will end with two jumps, but since the former is conditional this
			// is allowed in our basic block system, since our jumps don't have an on-false label.
			StmtLabel *fallthroughLabel = m_allocator->New<StmtLabel>("cond-jump-fallthrough");
			StmtJump *fallthroughJump = m_allocator->New<StmtJump>(fallthroughLabel, nullptr);

			fallthroughLabel->debugInfo.synthetic = true;
			fallthroughJump->debugInfo.synthetic = true;

			current->statements.push_back(fallthroughJump);
			current = CreateBasicBlock();
			current->statements.push_back(fallthroughLabel);

			continue;
		}
	}

	contents->statements = std::move(m_newContents);
}

StmtBlock *BasicBlockPass::CreateBasicBlock() {
	StmtBlock *block = m_allocator->New<StmtBlock>();
	block->isBasicBlock = true;
	block->debugInfo.synthetic = true;
	m_newContents.push_back(block);
	return block;
}
