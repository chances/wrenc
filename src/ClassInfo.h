//
// Created by znix on 19/07/22.
//

#pragma once

#include "SymbolTable.h"

#include <map>

class MethodInfo {
  public:
	~MethodInfo();

	Signature *signature = nullptr;
	bool isForeign = false;
	bool isStatic = false;
	int lineNum = 0; // The line at which this method was declared, used for error messages
	IRFn *fn = nullptr;
};

// Bookkeeping information for compiling a class definition.
class ClassInfo {
  public:
	~ClassInfo();

	// Returns whether this shares a name with a 'system class' line String implemented in C++
	// In this case, it should be blocked with only the rare exception of when we're compiling the
	// special wren_core module.
	bool IsSystemClass() const;

	// The name of the class.
	std::string name;

	// Attributes are currently not implemented, but this would be needed for them:
	// // Attributes for the class itself
	// ObjMap *classAttributes;
	// // Attributes for methods in this class
	// ObjMap *methodAttributes;

	// Symbol table for the fields of the class.
	SymbolTable fields;

	// Symbols for the methods defined by the class. Used to detect duplicate
	// method definitions.
	using MethodMap = std::map<Signature *, std::unique_ptr<MethodInfo>>;
	MethodMap methods;
	MethodMap staticMethods;

	// True if the class being compiled is a foreign class.
	bool isForeign;

	// True if the current method being compiled is static. This is false outside of the
	// parsing stage.
	bool inStatic;

	// The signature of the method currently being compiled. This is nullptr outside of
	// the parsing stage.
	Signature *signature;
};
