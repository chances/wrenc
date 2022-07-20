//
// Created by znix on 20/07/22.
//

#include "IRCleanup.h"

void IRCleanup::Process(IRNode *root) {
	Visit(root);

	// Remove all the unused labels
	for (const auto &entry : m_labelData) {
		const LabelInfo &info = entry.second;
		// If the label is used (so we shouldn't remove it) or it's parent isn't a block (so we can't remove it), then
		// skip this label.
		if (info.used || info.parent == nullptr)
			continue;

		// Search for and remove this label
		auto &stmt = info.parent->statements;
		for (int i = 0; i < stmt.size(); i++) {
			if (stmt.at(i) == entry.first) {
				stmt.erase(stmt.begin() + i);
				break;
			}
		}
	}
}

IRNode *IRCleanup::GetParent(int index) {
	// -1 would be the last item, -2 is it's parent - then any higher index gets further-up parents from there
	int id = m_parents.size() - 2 - index;
	if (id >= m_parents.size())
		return nullptr;
	return m_parents.at(id);
}

void IRCleanup::Visit(IRNode *node) {
	m_parents.push_back(node);
	IRVisitor::Visit(node);
	m_parents.pop_back();
}

void IRCleanup::VisitBlock(StmtBlock *node) {
	// If we flatten a block the statements list will get longer, but that's fine - how it is now we'll
	// go over all the nodes we flatten, so long as we decrement i after removing a block.
	for (int i = 0; i < node->statements.size(); i++) {
		IRStmt *stmt = node->statements.at(i);

		// Flatten nested blocks
		StmtBlock *nested = dynamic_cast<StmtBlock *>(stmt);
		if (nested) {
			// Replace the node with it's contents
			auto position = node->statements.begin() + i;
			node->statements.erase(position);
			node->statements.insert(position, nested->statements.begin(), nested->statements.end());

			// The current index now contains a new node, so process it again
			i--;
			continue;
		}

		Visit(stmt);
	}
}

void IRCleanup::VisitStmtLabel(StmtLabel *node) {
	// The label data may already been created by a jump earlier, so don't overwrite the used flag
	m_labelData[node].parent = dynamic_cast<StmtBlock *>(GetParent());

	IRVisitor::VisitStmtLabel(node);
}
void IRCleanup::VisitStmtJump(StmtJump *node) {
	if (node->target) {
		// This may add a new entry, if we're jumping forwards
		m_labelData[node->target].used = true;
	}
	IRVisitor::VisitStmtJump(node);
}
