//
// Created by znix on 10/07/2022.
//

#pragma once

#include "IRNode.h"

#include <string>

class FieldVariable;

/// Tracks the fields for a given class.
/// This notably does not track function signatures or anything like that.
class SymbolTable {
  public:
	~SymbolTable();

	/// Gets a given field, or allocates it if the field didn't already exist
	FieldVariable *Ensure(const std::string &name);

	std::vector<std::unique_ptr<FieldVariable>> fields;
	std::unordered_map<std::string, FieldVariable *> byName;
};

class FieldVariable {
  public:
	inline const std::string &Name() const { return m_name; }
	inline int Id() const { return m_fieldId; }

	~FieldVariable();

	/// If this is a static variable on a class that is known to only be declared
	/// once (as a counterexample, classes declared in a loop have their own
	/// versions of static variables for each declaration) then this is a pointer
	/// to that variable.
	std::unique_ptr<IRGlobalDecl> staticGlobal;

  private:
	FieldVariable();

	std::string m_name;
	int m_fieldId = 0;

	friend SymbolTable;
};
