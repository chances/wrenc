//
// Created by znix on 21/02/23.
//

#pragma once

#include "IRNode.h"

class ArenaAllocator;

class SSAPass {
  public:
	explicit SSAPass(ArenaAllocator *allocator);

	/// Convert the contents of a function to SSA form.
	/// Note the function MUST already be in basic block form!
	void Process(IRFn *fn);

  private:
	class VarScanner : public IRVisitor {
	  public:
		SSAPass *pass;
		StmtBlock *block = nullptr;
		void VisitStmtAssign(StmtAssign *node) override;
		void VisitExprLoad(ExprLoad *node) override;
		void VisitVar(VarDecl *var) override;
	};

	ArenaAllocator *m_allocator = nullptr;

	/// Used by the var-scanning pass to assign a unique ID suffix
	/// to each SSA variable name.
	int m_nextVarId;

	/// Called to look up a variable that should be imported into
	/// a block. This creates a Phi node if necessary.
	SSAVariable *ImportVariable(StmtBlock *block, LocalVariable *local, bool excludeBlock);

	/// Scan a block, running the scanner visitor on it.
	void Scan(StmtBlock *block);

	/// Remove all uses of a variable, replacing it with another.
	/// This is used when a 'trivial' Phi node is found, where it's
	/// inputs are all the same (excluding inputs that refer directly
	/// to it).
	static void RemoveTrivialPhi(SSAVariable *var, SSAVariable *replacement);

	/// Check if a Phi node is trivial, from the given input variables.
	/// If the node is trivial, it returns the one and only variable it contains. Otherwise null.
	static SSAVariable *IsPhiTrivial(SSAVariable *output, const std::vector<SSAVariable *> &inputs);
};
