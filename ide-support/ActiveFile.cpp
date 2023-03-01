//
// Support for parsing actively-changing files, and responding to queries
// based on that information.
//
// The information parsed out of the type-sitter tree is broken into 'scopes',
// each scope representing something like a class, method or a '{}' block
// of code. Each scope is built in isolation: neither it's parent nor it's
// children may affect it's contents. This is to allow for fast updates when
// the source code changes: we'll walk the old and new trees, and for each
// scope take a non-cryptographic hash of the tokens. We can thus easily and
// quickly figure out which scopes we can re-use, and which need to be
// parsed fresh.
//
// (Using ts_tree_get_changed_ranges for this would be better, but we want
//  to support the Godot editor, and that doesn't seem to have a clean way
//  to get the edited regions. This method always works.)
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

// Ugly, but don't add common to the include directories just
// for this one file.
#include "../common/HashUtil.h"

#include <assert.h>
#include <functional>
#include <string.h>
#include <string>

void ActiveFile::Update(TSTree *tree, const std::string &text) {
	m_currentTree = tree;
	m_contents = text;

	// We'll have to rebuild the scope mappings afterwards.
	m_scopeMappings.clear();

	// Set aside the old scopes for reuse. Note we can have multiple blocks
	// with the same hash if they're identical.
	std::unordered_map<uint64_t, std::vector<std::unique_ptr<AScope>>> oldScopes;
	for (std::unique_ptr<AScope> &scope : m_scopePool) {
		oldScopes[scope->hash].push_back(std::move(scope));
	}
	m_scopePool.clear();

	TSNode root = ts_tree_root_node(tree);
	TSTreeCursor cursor = ts_tree_cursor_new(root);

	// First, build a hash tree of the new tree. We'll compare this with
	// the old tree, to figure out which scopes need re-parsing.

	HashedScopeNode newTree = BuildScopeHashTree(&cursor, text);

	// Build the tree using either new or recycled scopes.
	std::function<AScope *(const HashedScopeNode &node, AScope *parent)> getScope;
	getScope = [&](const HashedScopeNode &node, AScope *parent) -> AScope * {
		ts_tree_cursor_reset(&cursor, node.node);

		// Reuse an existing scope if we can, otherwise re-parse it.
		AScope *scope;
		std::vector<std::unique_ptr<AScope>> &cached = oldScopes[node.hash];
		if (cached.empty()) {
			ideDebug("Parsing scope");
			scope = BuildScope(&cursor, 0);
			scope->hash = node.hash;
		} else {
			m_scopePool.push_back(std::move(cached.back()));
			scope = m_scopePool.back().get();
		}

		// Reset the fields that are changed after the scope is built, as
		// they will have been previously set.
		scope->parent = parent;
		scope->subScopes.clear();
		scope->classes.clear();

		m_scopeMappings[node.node.id] = scope;

		// Populate this node's parents
		for (const HashedScopeNode &childNode : node.children) {
			AScope *childScope = getScope(childNode, scope);
			scope->subScopes.push_back(childScope);

			if (childScope->classDef) {
				scope->classes[childScope->classDef->name] = childScope->classDef.get();
			}
		}

		return scope;
	};

	m_rootScope = getScope(newTree, nullptr);

	ts_tree_cursor_delete(&cursor);
}

ActiveFile::HashedScopeNode ActiveFile::BuildScopeHashTree(TSTreeCursor *cursor, const std::string &text) {
	struct HashNode {
		TSSymbol symbol;

		// The number of bytes this symbol is from the start of the scope,
		// or the newest scope - whichever is later. This will cause the
		// hash to change if anything in this block is moved around, but
		// not if one of the blocks inside it is changed.
		// The MSB is set to indicate this is the end of a block.
		// The symbol's field ID is them xored into it.
		// This probably won't wrap around (it's about 300 lines of
		// 100-byte-long lines), but if it does that doesn't matter.
		// The purpose of this is to identify when the whitespace inside
		// a block changes, so the old line numbers (relative to the block
		// start) are broken.
		// For a similar reason, the offset and field ID changes cancelling
		// each other out is very unlikely, without at least one other
		// symbol in the entire scope also changing.
		uint16_t blockOffset;
	};

	// Pick an arbitrary seed for hashing
	static const uint64_t SEED = hash_util::hashString("ide scope hashing", 0);

	// A big buffer where we'll put all the scope data to hash. If one scope
	// contains another, we keep putting that scope's data on, but pop it off
	// once that scope finishes.
	std::vector<uint8_t> buffer;

	std::vector<HashedScopeNode> scopes;
	scopes.push_back(HashedScopeNode{
	    .node = ts_tree_cursor_current_node(cursor),
	    .dataStartIdx = 0,
	});

	// The position relative to which identifier positions are measured.
	// There's no need to store this on a stack, since it's always either
	// the start of the current scope (if the scope hasn't found it's
	// first child yet) or the end position of the last child scope.
	int relativePositionBase = 0;

	bool isNodeExit = false;
	bool endOfChildren = false;

	while (true) {
		isNodeExit = endOfChildren;

		TSNode current = ts_tree_cursor_current_node(cursor);
		TSSymbol symbol = ts_node_symbol(current);
		TSFieldId fieldId = ts_tree_cursor_current_field_id(cursor);

		int position = ts_node_start_byte(current) - relativePositionBase;

		HashNode node = {
		    .symbol = symbol,
		    .blockOffset = (uint16_t)(position ^ fieldId),
		};

		if (!isNodeExit && ts_tree_cursor_goto_first_child(cursor)) {
			// Next iteration, we'll be visiting one of this node's children.
		} else if (ts_tree_cursor_goto_next_sibling(cursor)) {
			// No prefix change
		} else if (ts_tree_cursor_goto_parent(cursor)) {
			// Don't immediately go back to the same children again
			endOfChildren = true;
		} else {
			// End of the root node
			break;
		}

		if (isNodeExit) {
			node.blockOffset |= 0x8000;
		}

		// Identifiers and numbers are the only two nodes that represent
		// different things based on a difference in their content, not
		// in their children.
		int textStart = 0;
		int textLength = 0;
		if (symbol == grammarInfo->symIdentifier || symbol == grammarInfo->symNumber) {
			textStart = ts_node_start_byte(current);
			textLength = ts_node_end_byte(current) - textStart;
		}

		auto writeBlock = [&]() {
			int destPos = buffer.size();
			buffer.resize(buffer.size() + sizeof(node) + textLength);
			memcpy(buffer.data() + destPos, &node, sizeof(node));
			destPos += sizeof(node);
			memcpy(buffer.data() + destPos, text.data() + textStart, textLength);
		};
		writeBlock();

		// If we're entering or exiting a scope-splitting node, that means
		// setting up or finalising a scope.
		if (!IsScopeSplitter(symbol))
			continue;

		if (!endOfChildren) {
			// Entering a new scope

			// Create the scope block, and mark the point in the buffer that
			// contains the new scope's data.
			scopes.push_back(HashedScopeNode{
			    .node = current,
			    .fileOffset = (int)ts_node_start_byte(current),
			    .dataStartIdx = (int)buffer.size(),
			});

			relativePositionBase = ts_node_start_byte(current);

			// Write the node out a second time, so it appears in both the old
			// and new scope's hash. Don't include the file offset however - if
			// this scope is moved around inside the parent scope, we don't
			// need to re-compute it.
			node.blockOffset = 0;
			writeBlock();
		} else {
			// Exiting a scope

			// Hash all the current node data, and pop it off the buffer (which
			// we're basically using as a stack).
			int startPos = scopes.back().dataStartIdx;
			uint64_t hash = hash_util::hashData(buffer.data() + startPos, buffer.size() - startPos, SEED);
			buffer.resize(startPos);

			// Pop this scope off the stack, moving it to it's parent stack.
			HashedScopeNode scope = std::move(scopes.back());
			scope.hash = hash;
			scopes.pop_back();
			scopes.back().children.push_back(std::move(scope));

			// We're now back in the parent scope, anything we put on
			// the buffer will go into the parent scope's hash.
			// Don't bother putting a closing block into the parent
			// scope, it doesn't do anything useful.

			// Set the relative position counter, so this block changing
			// won't invalidate it's parent blocks.
			relativePositionBase = ts_node_end_byte(current);
		}
	}

	// Calculate the root scope's hash, which is just the hash of the remaining
	// data on the buffer - all the data from the scopes has been removed.
	scopes.back().hash = hash_util::hashData(buffer.data(), buffer.size(), SEED);

	// Hand out the root scope.
	return std::move(scopes.back());
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
	// If so, skip it - all the scopes are built separately when they change.
	if (IsScopeSplitter(sym)) {
		ideDebug("<child scope inserted here>");
		return;
	}

	// Main node processing.

	if (parentSym == grammarInfo->symVarDecl && fieldId == grammarInfo->fName) {
		std::string token = GetNodeText(current);
		scope->locals[token] = ALocalVariable{};
	}

	// Set the name for a class. This will be used later, for adding the class variable.
	if (parentSym == grammarInfo->symClassDef && fieldId == grammarInfo->fName) {
		scope->classDef->name = GetNodeText(current);
	}

	// Method handling
	if (grammarInfo->IsMethod(sym)) {
		scope->classDef->methods.push_back(AMethod{
		    .name = "<<UNNAMED>>",
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

	if (sym == grammarInfo->symClassDef) {
		scope->classDef = std::make_unique<AClassDef>();
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

bool ActiveFile::IsScopeSplitter(TSSymbol symbol) {
	return symbol == grammarInfo->symBlock || symbol == grammarInfo->symClassDef;
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

	auto suggestCompletion = [&](ACR::Entry entry) {
		// Check if this variable starts with what the user has already typed
		// TODO add fuzzy-searching, so the user doesn't have to type exactly this
		if (!entry.identifier.starts_with(nodeStr)) {
			ideDebug("  Found variable '%s', not a match.", entry.identifier.c_str());
			return;
		}

		ideDebug("  Found variable '%s', adding auto-complete entry.", entry.identifier.c_str());
		result.entries.push_back(entry);
	};

	for (AScope *scope = query.enclosingScope; scope; scope = scope->parent) {
		ideDebug("Scanning next scope for variables");

		for (const auto &entry : scope->locals) {
			suggestCompletion(ACR::Entry{
			    .identifier = entry.first,
			});
		}
		for (const auto &entry : scope->classes) {
			suggestCompletion(ACR::Entry{
			    .identifier = entry.first,
			});
		}

		if (scope->classDef) {
			ideDebug("  Found class '%s', entering:", scope->classDef->name.c_str());
			for (const AMethod &method : scope->classDef->methods) {
				suggestCompletion(ACR::Entry{
				    .identifier = method.name,
				});
			}
		}
	}
	ideDebug("Variable scope scanning complete");

	return result;
}
