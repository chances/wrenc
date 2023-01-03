//
// Created by znix on 20/07/22.
//

#include "IRCleanup.h"
#include "ArenaAllocator.h"
#include "CompContext.h"
#include "Scope.h"

#include <set>

IRCleanup::IRCleanup(ArenaAllocator *allocator) : m_allocator(allocator) {}

void IRCleanup::Process(IRNode *root) {
	VisitReadOnly(root);

	// Remove all the unused labels
	std::set<StmtBlock *> blocksToUpdate;
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

		// Make a note to clean this block up, in case you have two returns separated by a jump.
		blocksToUpdate.insert(info.parent);
	}

	for (StmtBlock *block : blocksToUpdate) {
		VisitBlock(block, false);
	}
}

IRNode *IRCleanup::GetParent(int index) {
	// -1 would be the last item, -2 is it's parent - then any higher index gets further-up parents from there
	int id = m_parents.size() - 2 - index;
	if (id >= m_parents.size())
		return nullptr;
	return m_parents.at(id);
}

void IRCleanup::VisitReadOnly(IRNode *node) {
	m_parents.push_back(node);
	IRVisitor::VisitReadOnly(node);
	m_parents.pop_back();
}

void IRCleanup::Visit(IRExpr *&node) {
	// Handle ExprRunStatements specially, since we flatten out it's statements and want to replace it with
	// a variable load node.
	// Note we don't return after running a substitution, since that substitution might not do anything and
	// thus need it's contents to be examined here.
	ExprRunStatements *runStatements = dynamic_cast<ExprRunStatements *>(node);
	if (runStatements) {
		node = SubstituteExprRunStatements(runStatements);
	}

	ExprFuncCall *funcCall = dynamic_cast<ExprFuncCall *>(node);
	if (funcCall) {
		node = SubstituteExprFuncCall(funcCall);
	}

	IRVisitor::Visit(node);
}

void IRCleanup::VisitBlock(StmtBlock *node) { VisitBlock(node, true); }

void IRCleanup::VisitBlock(StmtBlock *node, bool recurse) {
	RunStatementsTarget lastBlock = m_runStatementsTarget;
	m_runStatementsTarget = RunStatementsTarget{.block = node};

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

		if (!recurse)
			continue;

		m_runStatementsTarget.insertIndex = i;

		// Have to reset this, otherwise we'll end up in an infinite loop if it's ever set to true.
		m_runStatementsTarget.inserted = false;

		VisitReadOnly(stmt);

		// If one or more statements were inserted (before the one we just looked at now), then we need to stay
		// at the current loop index to avoid inspecting it again.
		// TODO this means visiting nodes twice, which is potentially a performance concern but shouldn't
		//  be a correctness one.
		if (m_runStatementsTarget.inserted)
			i--;

		// Remove empty StmtBeginUpvalues nodes - do this after visiting them,
		// since that removes all the non-upvalue variables.
		StmtBeginUpvalues *upvalues = dynamic_cast<StmtBeginUpvalues *>(stmt);
		if (upvalues) {
			if (upvalues->variables.empty()) {
				node->statements.erase(node->statements.begin() + i);

				// Another node has moved into this position, decrement i to iterate on it
				i--;
				continue;
			}
		}
	}

	// Remove returns *after* we're done flattening, since otherwise if there's a label in a block we wouldn't find it
	// For example, test/language/return/after_while.wren had this problem.
	for (int i = 0; i < node->statements.size(); i++) {
		IRStmt *stmt = node->statements.at(i);

		// If we find an unconditional branch, remove everything after it until we find a label, since that's the only
		// way you can access something after a return.
		if (stmt->IsUnconditionalBranch()) {
			while (i + 1 < node->statements.size()) {
				IRStmt *next = node->statements.at(i + 1);
				if (dynamic_cast<StmtLabel *>(next))
					break;
				node->statements.erase(node->statements.begin() + i + 1);
			}
		}
	}

	m_runStatementsTarget = lastBlock;
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

void IRCleanup::VisitFn(IRFn *node) {
	m_fnParents.push_back(node);
	IRVisitor::VisitFn(node);
	m_fnParents.pop_back();
}

void IRCleanup::VisitStmtBeginUpvalues(StmtBeginUpvalues *node) {
	// Remove all the variables without upvalues
	for (int i = node->variables.size() - 1; i >= 0; i--) {
		LocalVariable *var = node->variables.at(i);
		if (!var->upvalues.empty())
			continue;

		var->beginUpvalues = nullptr;
		node->variables.erase(node->variables.begin() + i);
	}

	IRVisitor::VisitStmtBeginUpvalues(node);
}

IRExpr *IRCleanup::SubstituteExprRunStatements(ExprRunStatements *node) {
	// Increment the insert index, so later substitutions will go after this one - if we didn't do this, multiple
	// ExprRunStatements would end up in the reverse order, as each one would be inserted before the previous one.
	int insertIdx = m_runStatementsTarget.insertIndex++;

	// Let VisitBlock know we've actually done something, so it knows to visit it
	m_runStatementsTarget.inserted = true;

	StmtBlock *target = m_runStatementsTarget.block;
	target->statements.insert(target->statements.begin() + insertIdx, node->statement);

	// Make a new load
	ExprLoad *load = m_allocator->New<ExprLoad>(node->temporary);
	load->debugInfo = node->debugInfo;
	return load;
}

IRExpr *IRCleanup::SubstituteExprFuncCall(ExprFuncCall *node) {
	// Function calls in assignments are fine - that's where we'd be moving it anyway.
	// Note we have to pass -1 to GetParent which is normally invalid, since our current node hasn't
	// been pushed onto the parents stack.
	IRNode *parent = GetParent(-1);
	if (dynamic_cast<StmtAssign *>(parent))
		return node;

	// Similarly, StmtEvalAndIgnore is fine - it only evaluates a single expression, so there can't be
	// any ordering issues. It'd also be a bit silly to make a temporary variable for it.
	if (dynamic_cast<StmtEvalAndIgnore *>(parent))
		return node;

	// Move the function call out, so that when ExprRunStatements are pulled out all the observed actions still
	// occur in the same order.

	// Make a temporary variable, and set it to the result of the call
	LocalVariable *tmpVar = m_allocator->New<LocalVariable>();
	tmpVar->name = "tmp_call_res_" + node->signature->name;
	m_fnParents.back()->locals.push_back(tmpVar);

	StmtAssign *newAssignment = m_allocator->New<StmtAssign>(tmpVar, node);
	newAssignment->debugInfo = node->debugInfo;

	int insertIdx = m_runStatementsTarget.insertIndex++;
	StmtBlock *target = m_runStatementsTarget.block;
	target->statements.insert(target->statements.begin() + insertIdx, newAssignment);
	m_runStatementsTarget.inserted = true;

	// Replace the call with a load from the variable
	ExprLoad *load = m_allocator->New<ExprLoad>(tmpVar);
	load->debugInfo = parent->debugInfo; // Use the parent's debug info, so we don't needlessly jump in the debugger
	return load;
}
