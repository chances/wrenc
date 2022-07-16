//
// Created by znix on 10/07/2022.
//

#pragma once

#include "CcValue.h"

#include <string>

// Declarations
class IRExpr;
class LocalVariable; // From Scope.h

// //////////////////// //
// //// INTERFACES //// //
// //////////////////// //

class VarDecl {
  public:
	// Describes where a variable is declared.
	enum ScopeType {
		// A local variable in the current function.
		SCOPE_LOCAL,

		// A local variable declared in an enclosing function.
		SCOPE_UPVALUE,

		// A top-level module variable.
		SCOPE_MODULE
	};

	virtual ~VarDecl();
	virtual std::string Name() const = 0;
	virtual ScopeType Scope() const = 0;
};

// //////////////////// //
// //// TOP-LEVEL  //// //
// //////////////////// //

class IRNode {};

class IRFn : public IRNode {};

class IRClass : public IRNode {};

/// Global variable declaration
class IRGlobalDecl : public IRNode, public VarDecl {
  public:
	std::string name;
	std::string Name() const override;
	ScopeType Scope() const override;
};

// //////////////////// //
// //// STATEMENTS //// //
// //////////////////// //

/**
 * Represents an action that can be executed at runtime.
 */
class IRStmt : public IRNode {};

/// Assign a value to a local or global variable
class StmtAssign : public IRStmt {
  public:
	StmtAssign() {}
	StmtAssign(VarDecl *var, IRExpr *expr) : var(var), expr(expr) {}

	VarDecl *var = nullptr;
	IRExpr *expr = nullptr;
};

// //////////////////// //
// //// EXPRESSIONS /// //
// //////////////////// //

class IRExpr : public IRNode {};

class ExprConst : public IRExpr {
  public:
	ExprConst();
	ExprConst(CcValue value);

	CcValue value;
};

class ExprLoad : public IRExpr {
  public:
	ExprLoad() = default;
	ExprLoad(VarDecl *var) : var(var) {}

	VarDecl *var;
};
