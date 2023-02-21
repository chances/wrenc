//
// Created by znix on 10/07/2022.
//

#pragma once

#include "IRNode.h"

#include <memory>
#include <string>
#include <unordered_map>

// Slightly odd place to put this, but since there's not really a good place to put this in the IR tree, leave
// it here.
class LocalVariable : public VarDecl {
  public:
	// Unsurprisingly, the name of this variable.
	std::string name;

	// If upvalues are bound to this variable, this contains the list of such variables.
	std::vector<UpvalueVariable *> upvalues;

	// If upvalues are bound to this variable, this contains the node representing
	// when this variable came into scope.
	StmtBeginUpvalues *beginUpvalues = nullptr;

	DLL_EXPORT std::string Name() const override;
	DLL_EXPORT ScopeType Scope() const override;
	DLL_EXPORT void Accept(IRVisitor *visitor) override;
};

/// Reference a variable from the enclosing function.
class UpvalueVariable : public VarDecl {
  public:
	DLL_EXPORT UpvalueVariable(VarDecl *parent, IRFn *fn) : parent(parent), containingFunction(fn) {}

	DLL_EXPORT std::string Name() const override;
	DLL_EXPORT ScopeType Scope() const override;
	DLL_EXPORT void Accept(IRVisitor *visitor) override;

	/// Upvalues can have either another upvalue or a local as their parent. This walks the parent
	/// chain until we find the local variable at the end of it.
	DLL_EXPORT LocalVariable *GetFinalTarget() const;

	/// The variable this upvalue references. Must either be a local variable or another upvalue import.
	VarDecl *parent = nullptr;

	/// The function this node belongs to. This is useful because you can find the upvalues of a local variable which
	/// naturally belongs to a different function.
	IRFn *containingFunction = nullptr;
};

class ScopeFrame {
  public:
	ScopeFrame *parent = nullptr;
	StmtBeginUpvalues *upvalueContainer = nullptr;
	std::unordered_map<std::string, LocalVariable *> locals;
};

class ScopeStack {
  public:
	ScopeStack();
	~ScopeStack();

	// Find a local variable by name, returning nullptr if it doesn't exist
	LocalVariable *Lookup(const std::string &name);

	// Add a local variable to the deepest scope. Returns true if the variable was added, or
	// false if a variable with the same name already existed in the deepest scope.
	bool Add(LocalVariable *var);

	// Get the total number of variables, including shadowed ones.
	int VariableCount();

	/// Get the specified stack frame, the top stack frame, and everything in between.
	/// This is mostly for jumping out of loops and returning, where you have to clear a bunch of stack frames
	/// not in the usual order.
	/// [since] is the ID of the first stack frame to include, from [GetTopFrame].
	std::vector<ScopeFrame *> GetFramesSince(int since);

	/// Get the index of the current stack frame.
	int GetTopFrame();

	void PopFrame();

	void PushFrame(StmtBeginUpvalues *upvalues);

  private:
	ScopeFrame *m_top = nullptr;
	std::vector<std::unique_ptr<ScopeFrame>> m_frames;
};
