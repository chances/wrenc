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
};

class FieldVariable {
  public:
	inline const std::string &Name() const { return m_name; }
	inline int Id() const { return m_fieldId; }

  private:
	~FieldVariable();
	FieldVariable();

	std::string m_name;
	int m_fieldId = 0;

	friend SymbolTable;
};
