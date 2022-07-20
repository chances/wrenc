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

	// The depth in the scope chain that this variable was declared at. Zero is
	// the outermost scope--parameters for a method, or the first local block in
	// top level code. One is the scope within that, etc.
	int depth = -1;

	// If this local variable is being used as an upvalue.
	bool isUpvalue = false;

	std::string Name() const override;
	ScopeType Scope() const override;
	void Accept(IRVisitor *visitor) override;
};

class ScopeFrame {
  public:
	ScopeFrame *parent = nullptr;
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

	void PopFrame();

	void PushFrame();

  private:
	ScopeFrame *m_top = nullptr;
	std::vector<std::unique_ptr<ScopeFrame>> m_frames;
};
