//
// Created by znix on 27/02/23.
//

#include "ActiveFile.h"
#include "GrammarInfo.h"
#include "ide_devtool.h"
#include "ide_util.h"

#include <string>

void ActiveFile::Update(TSTree *tree, const std::string &text) {
	m_currentTree = tree;
	m_contents = text;

	TSNode root = ts_tree_root_node(tree);
	TSTreeCursor cursor = ts_tree_cursor_new(root);

	std::string prefix;

	// TODO reuse what we can
	m_scopePool.clear();

	m_scopePool.push_back(std::make_unique<AScope>());
	m_rootScope = m_scopePool.back().get();

	// Walk over the entire tree, parsing what we can without backtracking.
	bool justExitedScope = false;
	std::vector<AScope *> stack;
	std::vector<TSSymbol> symbolStack;
	stack.push_back(m_rootScope);
	while (true) {
		TSNode current = ts_tree_cursor_current_node(&cursor);
		TSPoint start = ts_node_start_point(current);
		TSFieldId fieldId = ts_tree_cursor_current_field_id(&cursor);

		std::string extra;
		if (justExitedScope) {
			extra += " (EXIT)";
		} else {
			// Uncomment to print the node text - it's VERY verbose, since
			// each node includes the text of all it's child nodes.
			// printf(": %.*s", srcLength, srcStart);
		}

		ideDebug("%snode %d.%d '%s' (field='%s')%s", prefix.c_str(), start.row, start.column, ts_node_type(current),
		    ts_tree_cursor_current_field_name(&cursor), extra.c_str());

		if (!justExitedScope && !symbolStack.empty()) {
			// Main node processing.
			TSSymbol sym = ts_node_symbol(current);
			TSSymbol parent = symbolStack.back();

			if (sym == grammarInfo->symBlock) {
				// Entering a block

				AScope *prevTop = stack.back();

				// Allocate a new scope
				std::unique_ptr<AScope> uniqueScope = std::make_unique<AScope>();
				AScope *scope = uniqueScope.get();
				m_scopePool.push_back(std::move(uniqueScope));

				stack.push_back(scope);
				prevTop->subScopes.push_back(scope);

				m_scopeMappings[current.id] = scope;

				scope->parent = prevTop;
				scope->node = current;
			}

			if (parent == grammarInfo->symVarDecl && fieldId == grammarInfo->fName) {
				std::string token = GetNodeText(current);
				stack.back()->locals[token] = ALocalVariable{};
			}
		}

		if (justExitedScope) {
			// Exiting this scope

			// Check if this block corresponds to the previous node in the stack.
			if (!stack.empty() && ts_node_eq(stack.back()->node, current)) {
				stack.pop_back();
			}
		}

		bool madeExit = false;
		if (!justExitedScope && ts_tree_cursor_goto_first_child(&cursor)) {
			// Entering a scope
			prefix.push_back(' ');
			symbolStack.push_back(ts_node_symbol(current));
		} else if (ts_tree_cursor_goto_next_sibling(&cursor)) {
			// No prefix change
		} else if (ts_tree_cursor_goto_parent(&cursor)) {
			prefix.pop_back();
			symbolStack.pop_back();

			// Don't immediately go back to the same children again
			madeExit = true;
		} else {
			// End of the root node
			break;
		}
		justExitedScope = madeExit;
	}
}

APointQueryResult ActiveFile::PointQuery(TSPoint point) {
	TSNode root = ts_tree_root_node(m_currentTree);
	TSTreeCursor cursor = ts_tree_cursor_new(root);

	APointQueryResult result;

	// Each call descends one node deeper towards our target.
	while (true) {
		TSNode node = ts_tree_cursor_current_node(&cursor);
		result.nodes.push_back(node);

		auto iter = m_scopeMappings.find(node.id);
		if (iter != m_scopeMappings.end()) {
			result.enclosingScope = iter->second;
		}

		if (ts_tree_cursor_goto_first_child_for_point(&cursor, point) == -1) {
			break;
		}
	}

	return result;
}

std::string ActiveFile::GetNodeText(TSNode node) {
	// Read this identifier as a string
	const char *srcStart = m_contents.c_str() + ts_node_start_byte(node);
	int srcLength = ts_node_end_byte(node) - ts_node_start_byte(node);
	return std::string(srcStart, srcLength);
}

AutoCompleteResult ActiveFile::AutoComplete(TSPoint point) {
	using ACR = AutoCompleteResult;
	AutoCompleteResult result;

	// Look up information about this point - the nodes containing it, and
	// the scope it lives within.
	APointQueryResult query = PointQuery(point);

	// We currently only support auto-completing identifiers.
	if (ts_node_symbol(query.nodes.back()) != grammarInfo->symIdentifier) {
		ideDebug("stop auto complete: not an identifier");
		return result;
	}

	// Figure out what we're auto-completing.
	TSSymbol secondLastSym = ts_node_symbol(query.NodeRev(1));
	if (secondLastSym == grammarInfo->symVarLoad) {
		result.context = ACR::VARIABLE_LOAD;
	} else {
		// We're in an unsupported load context.
		ideDebug("stop auto complete: in unknown context '%s'", ts_node_type(query.NodeRev(1)));
		return result;
	}

	std::string nodeStr = GetNodeText(query.nodes.back());

	for (AScope *scope = query.enclosingScope; scope; scope = scope->parent) {
		ideDebug("Scanning next scope for variables");
		for (const auto &entry : scope->locals) {
			// Check if this variable starts with what the user has already typed
			// TODO add fuzzy-searching, so the user doesn't have to type exactly this
			if (!entry.first.starts_with(nodeStr)) {
				ideDebug("  Found variable '%s', not a match.", entry.first.c_str());
				continue;
			}

			ideDebug("  Found variable '%s', adding auto-complete entry.", entry.first.c_str());
			result.entries.push_back(ACR::Entry{
			    .identifier = entry.first,
			});
		}
	}
	ideDebug("Variable scope scanning complete");

	return result;
}
