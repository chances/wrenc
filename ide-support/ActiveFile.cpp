//
// Support for parsing actively-changing files, and responding to queries
// based on that information.
//
// The information parsed out of the type-sitter tree is broken into 'scopes',
// each scope representing something like a class, method or a '{}' block
// of code. Each scope is built in isolation: neither it's parent nor it's
// children may affect it's contents. This is to allow for fast updates when
// the source code changes: we walk the old and new tree-sitter trees at the
// same time, such that blocks of code that haven't changed between them can
// be identified. Those that are have the scope containing them re-parsed, and
// that replaces the old scope in the previous data structure.
//
// There's a few exceptions to this: perhaps most obviously, scopes have
// pointers to their parents and children, which are handled specially after
// they're built, and are updated during this process.
//
// Another exception is type inference. Consider the following:
//
//   var v = null    <- outer scope
//   if (thing) {
//     v = "Hello"   <- inner scope
//   }
//
// In this example, there's a variable defined in (which thus belongs to)
// the outer scope, however it's only assigned to an interesting type in
// the inner block. Thus the inner block records a set of 'contributions',
// which state how it affects 'v', without pointing to a specific block.
//
// When the inner block is updated, all the variables it affected are marked.
// Once the new inner block is in place, all the marked variables have their
// types re-calculated from the set of contributions. If that variable is then
// used to infer the types of other variables, this can be repeated until
// it propagates through everything. There will be some cases where only
// considering blocks in isolation prevents types from being inferred, and
// that can be delt with when any of the involved variables are used in a
// query - we only gather type information that's cheap, but the vast majority
// of variables won't be queried before the next edit.
//
// Created by znix on 27/02/23.
//

#include "ActiveFile.h"
#include "GrammarInfo.h"
#include "ide_devtool.h"
#include "ide_util.h"

#include <assert.h>
#include <string>

void ActiveFile::Update(TSTree *tree, const std::string &text) {
	m_currentTree = tree;
	m_contents = text;

	TSNode root = ts_tree_root_node(tree);
	TSTreeCursor cursor = ts_tree_cursor_new(root);

	std::string prefix;

	// TODO reuse what we can
	m_scopePool.clear();

	// Walk over the entire tree, parsing what we can without backtracking.
	m_rootScope = BuildScope(&cursor, 0);

	ts_tree_cursor_delete(&cursor);
}

void ActiveFile::WalkNodes(TSTreeCursor *cursor, AScope *scope, TSSymbol parentSym, int debugDepth) {
	TSNode current = ts_tree_cursor_current_node(cursor);
	TSPoint start = ts_node_start_point(current);
	TSFieldId fieldId = ts_tree_cursor_current_field_id(cursor);
	TSSymbol sym = ts_node_symbol(current);

	// TODO skip calculating the indentation if debug logging is off.
	std::string indentation;
	indentation.assign(debugDepth, ' ');
	ideDebug("%snode %d.%d '%s' (field='%s')", indentation.c_str(), start.row, start.column, ts_node_type(current),
	    ts_tree_cursor_current_field_name(cursor));

	// Check if this is one of the special nodes that create their own scope.
	if (sym == grammarInfo->symBlock || sym == grammarInfo->symClassDef) {
		AScope *nested = BuildScope(cursor, debugDepth);
		nested->parent = scope;
		scope->subScopes.push_back(nested);

		// Class definitions define a local corresponding to that class.
		// It's not really how wrenc works, but it is how Wren works
		// internally, and it's certainly fine to provide useful
		// completions.
		if (nested->classDef) {
			scope->locals[nested->classDef->name] = ALocalVariable{
			    .classDef = nested->classDef,
			};
		}

		return;
	}

	// Main node processing.

	if (parentSym == grammarInfo->symVarDecl && fieldId == grammarInfo->fName) {
		std::string token = GetNodeText(current);
		scope->locals[token] = ALocalVariable{};
	}

	// Set the name for a class. This will be used later, for adding the class variable.
	if (parentSym == grammarInfo->symClassDef && fieldId == grammarInfo->fName) {
		AClassDef *def = m_classPool.back().get();
		def->name = GetNodeText(current);
	}

	// Method handling
	if (grammarInfo->IsMethod(sym)) {
		scope->classDef->methods.push_back(AMethod{
		    .name = "<<UNNAMED>>",
		    .node = current,
		});
	}
	if (grammarInfo->IsMethod(parentSym) && fieldId == grammarInfo->fName) {
		scope->classDef->methods.back().name = GetNodeText(current);
	}

	// Loop through this node's children
	WalkChildren(cursor, scope, debugDepth);
}

AScope *ActiveFile::BuildScope(TSTreeCursor *cursor, int debugDepth) {
	TSNode current = ts_tree_cursor_current_node(cursor);
	TSSymbol sym = ts_node_symbol(current);

	// Allocate a new scope
	m_scopePool.push_back(std::make_unique<AScope>());
	AScope *scope = m_scopePool.back().get();

	m_scopeMappings[current.id] = scope;

	scope->node = current;

	if (sym == grammarInfo->symClassDef) {
		m_classPool.push_back(std::make_unique<AClassDef>());
		AClassDef *def = m_classPool.back().get();

		scope->classDef = def;
	}

	WalkChildren(cursor, scope, debugDepth);

	return scope;
}

void ActiveFile::WalkChildren(TSTreeCursor *cursor, AScope *scope, int debugDepth) {
	TSNode startNode = ts_tree_cursor_current_node(cursor);
	TSSymbol symbol = ts_node_symbol(startNode);

	// Try stepping into our first child node. If this fails, we don't have
	// any children and can stop here.
	if (!ts_tree_cursor_goto_first_child(cursor)) {
		return;
	}

	// Go through all our child nodes.
	while (true) {
		WalkNodes(cursor, scope, symbol, debugDepth + 1);

		// Advance to the next child, unless we're at the end of the list.
		if (!ts_tree_cursor_goto_next_sibling(cursor)) {
			break;
		}
	}

	// We must be able to step back out. This should only fail if the current
	// child is the root node, which is obviously impossible.
	if (!ts_tree_cursor_goto_parent(cursor)) {
		// TODO a proper ideAbort function that prints a prefix and everything.
		fprintf(stderr, "Couldn't step out of node during iteration!\n");
		abort();
	}

	// We should have ended up at the same node we started at. Anything else
	// and we'll break the next WalkChildren call (this is called by WalkNodes,
	// and is thus called recursively). Catch that and fail now, when it'll
	// hopefully be more obvious what happened.
	assert(ts_node_eq(startNode, ts_tree_cursor_current_node(cursor)));
}

APointQueryResult ActiveFile::PointQuery(TSPoint point) {
	TSNode root = ts_tree_root_node(m_currentTree);
	TSTreeCursor cursor = ts_tree_cursor_new(root);

	APointQueryResult result;

	// If we don't find any scope containing this node, we're in the
	// global scope.
	result.enclosingScope = m_rootScope;

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

		if (scope->classDef) {
			ideDebug("  Found class '%s', entering:", scope->classDef->name.c_str());
			for (const AMethod &method : scope->classDef->methods) {
				if (!method.name.starts_with(nodeStr)) {
					ideDebug("    Found method '%s', not a match.", method.name.c_str());
					continue;
				}

				ideDebug("    Found method '%s', adding auto-complete entry.", method.name.c_str());
				result.entries.push_back(ACR::Entry{
				    .identifier = method.name,
				});
			}
		}
	}
	ideDebug("Variable scope scanning complete");

	return result;
}
