//
// Created by znix on 10/07/2022.
//

#pragma once

#include "CcValue.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Declarations
class IRExpr;
class IRStmt;
class IRClass;
class LocalVariable;   // From Scope.h
class UpvalueVariable; // From Scope.h
class FieldVariable;   // From SymbolTable.h
class Signature;       // From CompContext.h
class Compiler;        // From wren_compiler.cpp
class ClassInfo;       // From ClassInfo.h
class MethodInfo;      // From ClassInfo.h
class IRVisitor;
class IRPrinter;
class StmtBlock;

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

	virtual void Accept(IRVisitor *visitor) = 0;
};

// An interface of random stuff for backends to use
class BackendNodeData {
  public:
	virtual ~BackendNodeData();
};

class IRDebugInfo {
  public:
	// The line number and column of the start (or thereabout) of the token that caused this node to be created.
	int lineNumber = -1;
	int column = -1;

	/// If true, this node was created by a pass or something like that, such that it doesn't have any real
	/// resemblance to the source code.
	bool synthetic = false;
};

// //////////////////// //
// //// TOP-LEVEL  //// //
// //////////////////// //

class IRNode {
  public:
	virtual ~IRNode();
	virtual void Accept(IRVisitor *visitor) = 0;

	// This pointer allows backends to attach whatever internal data they want to nodes
	std::unique_ptr<BackendNodeData> backendData;

	IRDebugInfo debugInfo;

	template <typename T> T *GetBackendData() const {
		T *data = dynamic_cast<T *>(backendData.get());
		if (!data) {
			fprintf(stderr, "Missing backend dynamic data\n");
			abort();
		}
		return data;
	}
};

class IRFn : public IRNode {
  public:
	void Accept(IRVisitor *visitor) override;

	std::vector<LocalVariable *> locals; // Locals may have duplicate names from different scopes, hence vector not map
	std::unordered_map<VarDecl *, UpvalueVariable *> upvalues; // Upvalues imported from the parent function
	std::unordered_map<std::string, UpvalueVariable *> upvaluesByName;

	// Function parameters. These also appear in [locals].
	std::vector<LocalVariable *> parameters;

	// Locals used as temporaries by the compiler, which aren't checked for name conflicts.
	std::vector<LocalVariable *> temporaries;

	// The closure functions defined inside this function. This is mainly for upvalue processing.
	std::vector<IRFn *> closures;

	// The thing that gets run when this function is called
	IRStmt *body = nullptr;

	// If this is a method, this is the class the method is contained in. If this is a closure or
	// the root function of a module, this is nullptr. This is a good way to check if this function
	// is a method or not.
	// Note this is also true for static methods (they are instance methods of the class object, and
	// have a normal receiver etc).
	IRClass *enclosingClass = nullptr;

	// If this is a method, this contains the information about that method.
	MethodInfo *methodInfo = nullptr;

	// If this function is a closure, then this is the function containing it.
	IRFn *parent = nullptr;

	// The name of this node, as it's used for debugging
	std::string debugName = "<unknown_func>";
};

class IRClass : public IRNode {
  public:
	~IRClass();
	void Accept(IRVisitor *visitor) override;

	std::unique_ptr<ClassInfo> info;

	/// Is this class defined somewhere other than the root of the module? This supports things like
	/// declaring classes in loops, where they might be declared any number of times.
	/// This is important for static variables, which have to be handled differently in dynamically
	/// defined classes, since using a global variable won't work since they're supposed to be separate.
	bool dynamicallyDefined = false;
};

/// Global variable declaration
class IRGlobalDecl : public IRNode, public VarDecl {
  public:
	void Accept(IRVisitor *visitor) override;

	std::string name;
	std::string Name() const override;
	ScopeType Scope() const override;

	/// If this variable hasn't been properly declared (eg it's used in a method, which is valid as it could
	/// be declared later in the file in the global scope) then this is set to the line number of the first
	/// line where it was used.
	std::optional<int> undeclaredLineUsed;

	/// If this global was created by declaring a class, this points to that class.
	IRClass *targetClass = nullptr;
};

/// Represents importing a given module. This forces the given module to be parsed and compiled.
/// Variables we import are bound to 'proxies': module-level variables are created, and when the
/// import is evaluated the proxies are set to the appropriate value from the imported module.
/// Note that it's legal to observe an imported variable before the import occurs:
///   class Foo {
///     static thing() {
///       System.print(Bar)
///     }
///   }
///   Foo.thing()
///   import "test2.wren" for Bar
/// In this case, the programme will print "null" since Bar hasn't been imported yet, and proxies
/// easily let us mirror that behaviour. When the "import" directive is actually executed it produces
/// a StmtLoadModule node which runs the module, if it hasn't already been loaded, and then sets
/// up the proxies.
///
/// There are also be cases where it's possible to (during our Wren 'linking' step, not the system
/// linker) recognise that a variable always has a specific value and optimise based on that, eg:
///  module1.wren:
///   var PI = 3.14
///   class A {
///     static fancyPrint(value) {
///       print(value)
///     }
///   }
///  module2.wren:
///   import "module1" for PI, A
///   A.fancyPrint(PI)
/// In this case it should be possible to compile down to a single print(3.14) call. For a variable
/// to be optimised like this, the following must be true:
/// * The variable is defined in module1 and never modified
/// * The variable must be imported to module2 before it's used
/// * We still have to follow import loop rules (though we can probably slack on those for now)
class IRImport : public IRNode {
  public:
	void Accept(IRVisitor *visitor) override;

	std::string moduleName;
};

// //////////////////// //
// //// STATEMENTS //// //
// //////////////////// //

/**
 * Represents an action that can be executed at runtime.
 */
class IRStmt : public IRNode {
  public:
	virtual bool IsUnconditionalBranch();

	/// If this statement is contained within a basic block, then this points to that block.
	StmtBlock *basicBlock = nullptr;
};

/// Assign a value to a local or global variable
class StmtAssign : public IRStmt {
  public:
	StmtAssign() {}
	StmtAssign(VarDecl *var, IRExpr *expr) : var(var), expr(expr) {}

	void Accept(IRVisitor *visitor) override;

	VarDecl *var = nullptr;
	IRExpr *expr = nullptr;
};

/// Assign a value to the receiver object's fields
class StmtFieldAssign : public IRStmt {
  public:
	StmtFieldAssign() {}
	StmtFieldAssign(FieldVariable *var, IRExpr *value) : var(var), value(value) {}

	void Accept(IRVisitor *visitor) override;

	FieldVariable *var = nullptr;
	IRExpr *value = nullptr;
};

/// Statement that evaluates an expression and throws away the result. This is an adapter of sorts for IRExpr-s.
class StmtEvalAndIgnore : public IRStmt {
  public:
	StmtEvalAndIgnore(IRExpr *expr) : expr(expr) {}

	void Accept(IRVisitor *visitor) override;

	IRExpr *expr = nullptr;
};

/// A group of statements. Mainly for returning multiple statements as a single pointer.
class StmtBlock : public IRStmt {
  public:
	/// Adds a statement, doing nothing if it's nullptr.
	void Add(IRStmt *stmt);

	void Accept(IRVisitor *visitor) override;

	std::vector<IRStmt *> statements;

	/// Is this block a basic block? This means the block starts with a label and ends with a control flow
	/// statement (either a jump or a return). Blocks are converted to basic blocks by the basic block pass.
	/// The only exception is for conditional jumps: then the last two statements must both be jumps, first
	/// a conditional jump then an unconditional jump.
	bool isBasicBlock = false;
};

/// Not really a statement, this designates a point the jump instruction can jump to
class StmtLabel : public IRStmt {
  public:
	StmtLabel(std::string debugName) : debugName(std::move(debugName)) {}

	void Accept(IRVisitor *visitor) override;

	std::string debugName;
};

/// Jump to a label, possibly conditionally.
class StmtJump : public IRStmt {
  public:
	StmtJump(StmtLabel *target, IRExpr *condition) : target(target), condition(condition) {}
	StmtJump() = default;

	void Accept(IRVisitor *visitor) override;

	bool IsUnconditionalBranch() override;

	StmtLabel *target = nullptr;
	IRExpr *condition = nullptr; /// Unconditional if nullptr. Otherwise, if it evaluates to null or false, won't jump.
	bool looping = false; /// If this jump is part of a loop, this is true. Otherwise the jump MUST only go 'forwards'.
	bool jumpOnFalse = false; /// Jump if this value is falsy, and fallthrough on truey. Opposite of normal behaviour.
};

class StmtReturn : public IRStmt {
  public:
	explicit StmtReturn(IRExpr *value) : value(value) {}

	void Accept(IRVisitor *visitor) override;

	bool IsUnconditionalBranch() override;

	IRExpr *value = nullptr;
};

/// Forces a module's main function to be run. See [IRImport].
class StmtLoadModule : public IRStmt {
  public:
	struct VarImport {
		std::string name; // Name in the module we're importing from
		VarDecl *bindTo = nullptr;
	};

	void Accept(IRVisitor *visitor) override;

	// The import this load triggers
	IRImport *importNode = nullptr;

	// The variables to import by name
	std::vector<VarImport> variables;
};

/// Moves an upvalue (if it's currently referenced by a closure) from the
/// stack to the heap, and updates all the closures accordingly.
/// This is placed at the end of source-level blocks so variables modified
/// in later iterations of a loop don't alter the value of the variables
/// defined in previous iterations.
class StmtRelocateUpvalues : public IRStmt {
  public:
	void Accept(IRVisitor *visitor) override;

	std::vector<LocalVariable *> variables;
};

/// Define a class. This is matched up to a class definition in Wren, and
/// at this point the superclass expression is ready.
/// For dynamically-defined classes, this can be repeated in a loop or similar.
class StmtDefineClass : public IRStmt {
  public:
	void Accept(IRVisitor *visitor) override;

	IRClass *targetClass = nullptr;

	/// The variable to store the class object in. This must not be null - even
	/// for top-level classes that can also be accessed via ExprGetClassVar, the
	/// class object must be stored somewhere for GC purposes.
	VarDecl *outputVariable = nullptr;
};

// //////////////////// //
// //// EXPRESSIONS /// //
// //////////////////// //

class IRExpr : public IRNode {};

class ExprConst : public IRExpr {
  public:
	ExprConst();
	ExprConst(CcValue value);

	void Accept(IRVisitor *visitor) override;

	CcValue value;
};

class ExprLoad : public IRExpr {
  public:
	ExprLoad() = default;
	ExprLoad(VarDecl *var) : var(var) {}

	void Accept(IRVisitor *visitor) override;

	VarDecl *var;
};

/// Read a value from the receiver object's fields
class ExprFieldLoad : public IRExpr {
  public:
	ExprFieldLoad() {}
	ExprFieldLoad(FieldVariable *var) : var(var) {}

	void Accept(IRVisitor *visitor) override;

	FieldVariable *var = nullptr;
};

/// Either a function or a method call, depending on whether the receiver is null or not
class ExprFuncCall : public IRExpr {
  public:
	void Accept(IRVisitor *visitor) override;

	Signature *signature = nullptr; /// The signature of the method to call. MUST be unique-ified by CompContext
	std::vector<IRExpr *> args;     /// The list of arguments to pass, must match the function's arity at runtime
	IRExpr *receiver = nullptr; /// Object the method will be called on. Null is valid and indicates a function call.

	/// If this call is a super call (a call to a parent object's implementation of a function), this is the
	/// method either in which this call was made, or if this call was made in a closure, then the method that the
	/// closure was defined in. Nested closures all point back to the method the outer-most one was declared in.
	IRFn *superCaller = nullptr;
};

/// Create a closure over a function, binding any upvalues. This is used even when there are no upvalues, and
/// if optimisations are performed on that it won't be during parsing.
class ExprClosure : public IRExpr {
  public:
	void Accept(IRVisitor *visitor) override;

	IRFn *func = nullptr;
};

/// Returns the 'this' value.
class ExprLoadReceiver : public IRExpr {
  public:
	void Accept(IRVisitor *visitor) override;
};

/// Run a collection of statements to initialise a temporary variable, which is then
/// used as the result of this expression.
/// This is to be used for things like list initialisers and as such MUST NOT return, break
/// or jump outside of itself (jumps between points inside the block are fine though, eg if statements).
class ExprRunStatements : public IRExpr {
  public:
	void Accept(IRVisitor *visitor) override;

	IRStmt *statement = nullptr;
	LocalVariable *temporary = nullptr;
};

/// Allocates the memory for a new object. If this is a foreign object, the foreign allocation method
/// is also called.
class ExprAllocateInstanceMemory : public IRExpr {
  public:
	void Accept(IRVisitor *visitor) override;

	IRClass *target = nullptr;
};

/// Get a built-in variable, for example the Object class.
class ExprSystemVar : public IRExpr {
  public:
	ExprSystemVar(std::string name);
	void Accept(IRVisitor *visitor) override;

	std::string name;

	// The variable names and their IDs
	static const std::unordered_map<std::string, int> SYSTEM_VAR_NAMES;
};

/// Get an ObjClass object declared in the current module by name
class ExprGetClassVar : public IRExpr {
  public:
	void Accept(IRVisitor *visitor) override;
	IRClass *cls = nullptr;
};

// //////////////////// //
// ////   VISITOR   /// //
// //////////////////// //

/// Visitor that walks the AST tree and sees every node. Extend it to make passes over the AST tree.
/// Every node should only be walked once, and variables may be walked multiple times (as often as
/// they're referenced.
class IRVisitor {
  public:
	virtual ~IRVisitor();

	virtual void VisitReadOnly(IRNode *node); // Can't switch out the referenced node, try to avoid this

	// Visit takes a reference to a pointer: this allows the visitor to overwrite the pointer passed in. These
	// should be called with a reference to the pointer stored in the parent node, so that if the visitor
	// changes the expression/statement then the node will be updated appropriately.
	// This is required to let visitors substitute one node for another without having to handle all the
	// possible nodes that could contain the one it wants to substitute.
	virtual void Visit(IRExpr *&node);
	virtual void Visit(IRStmt *&node);

	virtual void VisitVar(VarDecl *var);

	virtual void VisitFn(IRFn *node);
	virtual void VisitClass(IRClass *node);
	virtual void VisitGlobalDecl(IRGlobalDecl *node);
	virtual void VisitImport(IRImport *node);
	virtual void VisitStmtAssign(StmtAssign *node);
	virtual void VisitStmtFieldAssign(StmtFieldAssign *node);
	virtual void VisitStmtEvalAndIgnore(StmtEvalAndIgnore *node);
	virtual void VisitBlock(StmtBlock *node);
	virtual void VisitStmtLabel(StmtLabel *node);
	virtual void VisitStmtJump(StmtJump *node);
	virtual void VisitStmtReturn(StmtReturn *node);
	virtual void VisitStmtLoadModule(StmtLoadModule *node);
	virtual void VisitExprConst(ExprConst *node);
	virtual void VisitExprLoad(ExprLoad *node);
	virtual void VisitExprFieldLoad(ExprFieldLoad *node);
	virtual void VisitExprFuncCall(ExprFuncCall *node);
	virtual void VisitExprClosure(ExprClosure *node);
	virtual void VisitExprLoadReceiver(ExprLoadReceiver *node);
	virtual void VisitExprRunStatements(ExprRunStatements *node);
	virtual void VisitExprAllocateInstanceMemory(ExprAllocateInstanceMemory *node);
	virtual void VisitExprSystemVar(ExprSystemVar *node);
	virtual void VisitExprGetClassVar(ExprGetClassVar *node);
	virtual void VisitStmtRelocateUpvalues(StmtRelocateUpvalues *node);
	virtual void VisitStmtDefineClass(StmtDefineClass *node);

	virtual void VisitLocalVariable(LocalVariable *var);
	virtual void VisitUpvalueVariable(UpvalueVariable *var);
	// TODO for other variables
};
