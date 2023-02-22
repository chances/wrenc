//
// Implements a variable-to-SSA pass, using the algorithm set out in the paper:
// Simple and Efficient Construction of Static Single Assignment Form, Braun et al.
// https://doi.org/10.1007/978-3-642-37051-9_6
//
// Created by znix on 21/02/23.
//

#include "SSAPass.h"
#include "ArenaAllocator.h"
#include "Scope.h"

#include <algorithm>
#include <assert.h>
#include <functional>
#include <set>
#include <unordered_map>

class PhiOffer;

class BlockInfo : public BackendNodeData {
  public:
	// The function this block belongs to
	IRFn *fn = nullptr;

	std::vector<StmtBlock *> successors;   // aka 'children', blocks that this block jumps to.
	std::vector<StmtBlock *> predecessors; // aka 'parents', blocks that jump to this block.

	/// Statements to be prepended to this block. Adding them here means they're all prepended
	/// at once, which is faster than doing them one-at-a-time.
	std::vector<IRStmt *> prepend;

	/// The list of local variables set by this block, and what SSA variables they map into.
	/// If a variable is assigned multiple times by a block, this points to the variable
	/// that is active when the block ends.
	/// As SSA variables are 'pulled through' blocks that don't use them, this also stores
	/// all of those as a sort of cache.
	std::unordered_map<LocalVariable *, SSAVariable *> exports;

	/// The Phi nodes that could be created in this block, if required. See the PhiOffer comment.
	std::unordered_map<LocalVariable *, PhiOffer *> offers;

	/// All the variable reads that use variables set out of this block (these are the ones
	/// requiring Global Value Numbering, to use the paper's terminology).
	std::vector<ExprLoad *> loads;

	/// This block's position in the non-reversed postorder sequence.
	int position = -1;

	/// If true, this block either is being or has been scanned with
	/// the recursive SSA algorithm.
	bool scanned = false;
};

class LocalInfo : public BackendNodeData {
  public:
};

// Set on SSA variables
class SSAInfo : public BackendNodeData {
  public:
	IRFn *fn = nullptr;
	std::vector<ExprPhi *> phiUsers;
	std::vector<ExprLoad *> loadUsers;

	// If this variable has been replaced by another during redundant
	// Phi node cleanup, this points to the new variable.
	SSAVariable *replacement = nullptr;
};

class PhiOffer {
  public:
	LocalVariable *target = nullptr;

	std::function<SSAVariable *()> producer;
};

// Avoid all the dynamic casts for performance, since we already
// know what the types will be.
static BlockInfo *getBI(IRStmt *stmt) {
	StmtBlock *block = (StmtBlock *)stmt;
	return (BlockInfo *)block->backendData.get();
}

static SSAInfo *getSI(SSAVariable *var) { return (SSAInfo *)var->backendVarData.get(); }

template <typename T> static void listRemove(std::vector<T> &list, const T &item) {
	// std::remove moves all the new values to the beginning, and returns
	// the new end iterator.
	auto newLast = std::remove(list.begin(), list.end(), item);
	size_t newLength = newLast - list.begin();
	list.resize(newLength);
}

SSAPass::SSAPass(ArenaAllocator *allocator) : m_allocator(allocator) {}

void SSAPass::Process(IRFn *fn) {
	m_nextVarId = 1;

	// Use the backend pointer of the local variables to store
	// our variable information.
	for (LocalVariable *local : fn->locals) {
		// Don't touch anything related to upvalues, since writes
		// are externally observable.
		// Also, module imports need a way to keep the local as a
		// local so the backend can modify it without an assignment.
		if (local->upvalues.empty() && !local->disableSSA) {
			local->backendVarData = std::make_unique<LocalInfo>();
		} else {
			local->backendVarData.reset();
		}
	}

	// Go through and create the SSA variables for all the assignments,
	// and handle variables that are used in the same block as they're set.
	for (IRStmt *blockStmt : fn->body->statements) {
		StmtBlock *block = dynamic_cast<StmtBlock *>(blockStmt);
		if (!block) {
			fprintf(stderr, "Found non-block statement in SSA pass: '%s'\n", typeid(blockStmt).name());
			abort();
		}
		if (!block->isBasicBlock) {
			fprintf(stderr, "Found non-basic block in SSA pass!\n");
			abort();
		}
		block->backendData = std::make_unique<BlockInfo>();
		getBI(block)->fn = fn;
	}

	// Calculate the predecessors, now that all the BlockInfo-s exist.
	for (IRStmt *blockStmt : fn->body->statements) {
		StmtBlock *block = (StmtBlock *)blockStmt;

		// There can be more than one jump at the end in the case of a conditional
		// jump, so loop back until we find a non-jump.
		for (int i = block->statements.size() - 1; i >= 0; i--) {
			StmtJump *jump = dynamic_cast<StmtJump *>(block->statements.at(i));
			if (!jump)
				break;

			getBI(block)->successors.push_back(block);
			getBI(jump->target->basicBlock)->predecessors.push_back(block);
		}

		// While we're here, analyse the block and run local variable numbering on it.
		// We have to do this on all the blocks before we can start scanning any of them, so
		// that all the set variables are properly exported.
		VarScanner scanner;
		scanner.block = block;
		scanner.pass = this;
		scanner.VisitBlock(block);
	}

	// Run the visitor on each block in turn, filling it in.
	for (IRStmt *blockStmt : fn->body->statements) {
		StmtBlock *block = (StmtBlock *)blockStmt;
		Scan(block);
	}

	// Finally, prepend all the Phi nodes that were created.
	for (IRStmt *blockStmt : fn->body->statements) {
		StmtBlock *block = (StmtBlock *)blockStmt;
		BlockInfo *bi = getBI(block);

		if (block->statements.empty())
			continue;

		block->statements.insert(block->statements.begin(), bi->prepend.begin(), bi->prepend.end());

		// Only fill in the SSA input data now that it's actually required.
		// This lets us skip filling it into blocks that don't have Phi nodes.
		block->ssaInputs = bi->predecessors;
	}
}

SSAVariable *SSAPass::ImportVariable(StmtBlock *block, LocalVariable *local, bool excludeBlock) {
	BlockInfo *bi = getBI(block);

	// If this function is being called directly to find a variable used
	// in a block, don't look at said block's exports - for example:
	//
	//   i1 = i? + 123
	//
	// If we did scan that block, we'd use the exported 'i1' as 'i?', rather than
	// recursively importing it from the previous block.
	if (!excludeBlock) {
		Scan(block);

		// Check if we already export this value.
		auto exportIter = bi->exports.find(local);
		if (exportIter != bi->exports.end()) {
			return exportIter->second;
		}

		// Check if there's an offer on this value, offering to create
		// a Phi node on-demand. This is how we prevent infinite recursion
		// on the back-edge jumps of a loop.
		auto offerIter = bi->offers.find(local);
		if (offerIter != bi->offers.end()) {
			return offerIter->second->producer();
		}
	}

	// Create a Phi node offer, in case a later import needs to grab our variable.
	PhiOffer offer;
	bi->offers[local] = &offer;

	// The offer lazily creates the output SSA variable, which we'll set later.
	SSAVariable *producerResult = nullptr;
	offer.producer = [&]() -> SSAVariable * {
		if (producerResult)
			return producerResult;

		producerResult = m_allocator->New<SSAVariable>();
		producerResult->backendVarData = std::make_unique<SSAInfo>();
		producerResult->GetBackendVarData<SSAInfo>()->fn = bi->fn;
		producerResult->name = local->name + "_phi" + std::to_string(m_nextVarId++);
		producerResult->local = local;
		bi->fn->ssaVars.push_back(producerResult);
		return producerResult;
	};

	std::vector<SSAVariable *> vars;
	vars.reserve(bi->predecessors.size());

	// Try importing from all the predecessors.
	for (StmtBlock *pred : bi->predecessors) {
		SSAVariable *var = ImportVariable(pred, local, false);
		vars.push_back(var);
	}

	// Now all the imports (and thus replacements) are done, update any variables
	// that were replaced and figure out if we need a Phi node here or not.
	for (SSAVariable *&var : vars) {
		while (true) {
			SSAVariable *replacement = getSI(var)->replacement;
			if (!replacement)
				break;
			var = replacement;
		}
	}

	// If the Phi node is trivial, this returns the variable it's equivalent to.
	SSAVariable *result = IsPhiTrivial(producerResult, vars);

	if (result == nullptr) {
		// We need a Phi node here, make the variable if it hasn't already
		// been created.
		result = offer.producer();

		assert(getSI(result)->replacement == nullptr);

		// Make the Phi node itself, which is just an expression.
		ExprPhi *phi = m_allocator->New<ExprPhi>();
		phi->debugInfo.synthetic = true;
		phi->inputs = std::move(vars);

		for (SSAVariable *input : phi->inputs) {
			assert(getSI(input)->replacement == nullptr);
			input->GetBackendVarData<SSAInfo>()->phiUsers.push_back(phi);
		}

		// And assign that to the new variable.
		StmtAssign *assignment = m_allocator->New<StmtAssign>(result, phi);
		assignment->debugInfo.synthetic = true;
		assignment->basicBlock = block;
		result->assignment = assignment;
		phi->assignment = assignment;
		bi->prepend.push_back(assignment);
	} else {
		// If this 'new Phi node' is being used by other Phi nodes, get
		// rid of it and replace it with the new value. Do this recursively
		// over any other Phi nodes that become trivial.
		if (producerResult) {
			RemoveTrivialPhi(producerResult, result);
		}

		// The variable we obtained might have been replaced with
		// another during trivial Phi optimisation at some point.
		while (getSI(result)->replacement) {
			result = getSI(result)->replacement;
		}
	}

	assert(getSI(result)->replacement == nullptr);

	// Be sure to remove our offer, since it's going to leave the stack!
	bi->offers.erase(local);

	// If this value is already in the exports table, then that belongs to
	// a value computed inside this basic block, so don't overwrite it.
	if (!bi->exports.contains(local)) {
		bi->exports[local] = result;
	}
	return result;
}

void SSAPass::RemoveTrivialPhi(SSAVariable *var, SSAVariable *replacement) {
	// Note we take a variable not a Phi node, as this is called by ImportVariable to optimise
	// a Phi node that hasn't been created, but it's variable has been.

	assert(replacement);

	SSAInfo *info = getSI(var);
	SSAInfo *repInfo = getSI(replacement);
	assert(info->replacement == nullptr);
	assert(repInfo->replacement == nullptr);
	info->replacement = replacement;

	// Replace all the loads that use this variable.
	for (ExprLoad *load : info->loadUsers) {
		load->var = replacement;
	}
	repInfo->loadUsers.insert(repInfo->loadUsers.end(), info->loadUsers.begin(), info->loadUsers.end());

	// For all the Phi nodes that use this node, replace it and check if they're not redundant.
	for (ExprPhi *phi : info->phiUsers) {
		StmtAssign *assignment = phi->assignment;

		// This Phi node does assign to an SSA variable, right?
		SSAVariable *output = dynamic_cast<SSAVariable *>(assignment->var);
		if (output == nullptr) {
			std::string name = assignment->var->Name();
			fprintf(stderr, "[SSAPass] Phi node writes to non-SSA variable '%s'\n", name.c_str());
			abort();
		}

		// Replace all instances of the variable in this Phi node's input.
		std::replace(phi->inputs.begin(), phi->inputs.end(), var, replacement);

		SSAVariable *remaining = IsPhiTrivial(output, phi->inputs);
		if (remaining == nullptr) {
			continue;
		}

		// This Phi node is redundant, eliminate it.
		BlockInfo *bi = getBI(assignment->basicBlock);
		listRemove<IRStmt *>(bi->prepend, assignment);

		// Remove this node from the user lists, so others won't try removing it again later.
		listRemove(getSI(remaining)->phiUsers, phi);

		// Replace all usages of it's output variable.
		RemoveTrivialPhi(output, remaining);
	}

	IRFn *fn = info->fn;
	listRemove(fn->ssaVars, var);
}

SSAVariable *SSAPass::IsPhiTrivial(SSAVariable *output, const std::vector<SSAVariable *> &inputs) {
	// Replace and search this Phi node's inputs, to see if it's safe to replace.
	SSAVariable *only = nullptr;
	bool hasMultiple = false;
	for (SSAVariable *var : inputs) {
		if (var == output) {
			// If this Phi node takes itself and one other value as an
			// input, then it can be replaced with that one other value.
			// Thus don't update or check 'only' here.
		} else if (only == nullptr) {
			only = var;
		} else if (only != var) {
			hasMultiple = true;
		}
	}

	// This Phi node is still required if it has multiple distinct inputs.
	if (hasMultiple) {
		return nullptr;
	}

	assert(only != nullptr);

	// Return the one remaining input variable.
	return only;
}

void SSAPass::Scan(StmtBlock *block) {
	// This function is called by VisitBlock recursively, so set
	// the scanned flag *before* calling VisitBlock.
	BlockInfo *bi = getBI(block);
	if (bi->scanned)
		return;
	bi->scanned = true;

	// 'import' this variable - this digs through all the parent nodes and
	// creates a Phi node, if necessary.
	// We need a small cache for imported variables, since ImportVariable
	// won't search our exports table since we told it not to (with
	// the excludeBlock argument), because we could then load values
	// computed inside this block.
	// The cache thus saves creating a new Phi node for each time this
	// variable is used, if this block should contain that node.
	std::unordered_map<LocalVariable *, SSAVariable *> cache;
	for (ExprLoad *node : bi->loads) {
		LocalVariable *local = dynamic_cast<LocalVariable *>(node->var);

		auto iter = cache.find(local);
		SSAVariable *var;
		if (iter == cache.end()) {
			var = ImportVariable(block, local, true);
			cache[local] = var;
		} else {
			var = iter->second;
		}

		node->var = var;
		assert(getSI(var)->replacement == nullptr);
		var->GetBackendVarData<SSAInfo>()->loadUsers.push_back(node);
	}
}

void SSAPass::VarScanner::VisitStmtAssign(StmtAssign *node) {
	// Visit the expression first, in case we're loading from the same
	// variable as we're assigning to.
	Visit(node->expr);

	LocalVariable *lv = dynamic_cast<LocalVariable *>(node->var);
	if (!lv) {
		return;
	}

	// If a variable is used as an upvalue or isn't a local, it won't have
	// it's LocalInfo set, and thus we should ignore it.
	if (!node->var->backendVarData) {
		return;
	}

	// Each write gets it's own new SSA variable, which the reads
	// will later be modified to use.
	SSAVariable *var = pass->m_allocator->New<SSAVariable>();
	var->backendVarData = std::make_unique<SSAInfo>();
	var->assignment = node;
	node->var = var;

	// Add a suffix to the name, so we can separate different SSA variables
	// that are all attached to the same local variable.
	var->name = lv->name + "_ssa" + std::to_string(pass->m_nextVarId++);

	BlockInfo *bi = getBI(block);
	bi->exports[lv] = var;
	bi->fn->ssaVars.push_back(var);
}

void SSAPass::VarScanner::VisitExprLoad(ExprLoad *node) {
	// We don't need to do anything with non-local variables.
	LocalVariable *local = dynamic_cast<LocalVariable *>(node->var);
	if (!local) {
		return;
	}

	// If a variable is used as an upvalue or isn't a local, it won't have
	// it's LocalInfo set, and thus we should ignore it.
	if (!node->var->backendVarData) {
		return;
	}

	// Mark this read, we might use that information later?
	// FIXME varInfo->reads.push_back(node);

	// Check if we've already either written to or looked up this variable
	// in this block already.
	BlockInfo *bi = getBI(block);
	auto iter = bi->exports.find(local);
	if (iter != bi->exports.end()) {
		node->var = iter->second;
		return;
	}

	// Put this load on our list of loads to fix up later.
	bi->loads.push_back(node);
}

void SSAPass::VarScanner::VisitVar(VarDecl *var) {
	IRVisitor::VisitVar(var);

	LocalVariable *local = dynamic_cast<LocalVariable *>(var);

	// Module imports are special, since they overwrite variables by name.
	if (local && local->disableSSA) {
		return;
	}

	// All loads/stores to local variables should be handled through ExprLoad and StmtAssign.
	// This restriction makes things a bit easier here, since we don't have to special-case
	// module imports and classes.
	if (local) {
		std::string name = var->Name();
		fprintf(stderr, "Found local variable used in non-load, non-store node: '%s'\n", name.c_str());
		abort();
	}
}
