//
// The type inference pass - this figures out what type every variable is. This
// is desirable, since it's the backbone of getting rid of virtual method
// dispatches and allowing inlining. The difference between a floating point
// addition and a virtual dispatch to Num.+(_) for example is huge.
//
// Without type annotations this will necessarily imperfect, but it must never
// be inaccurate. A variable can have it's type set to null, which means it's
// unknown. Similarly, marking a variable as nullable is always legal, but
// having a null variable which is not marked as nullable is not.
//
// Doing this type resolution is surprisingly hard. Consider the following
// trivial loop:
//
//   var i = 1
//   while (i<1000) {
//     i = i + 1
//   }
//   System.print(i)
//
// In SSA form, this becomes:
//
//  var a = 1
//  while (true) {
//    var b = phi(a, c)
//    if (b >= 1000)
//      break
//    var c = b + 1
//  }
//  System.print(c)
//
// Obviously, a,b,c are all integers. For a it's easy, but finding the type
// of b requires knowing the type of c (if, for example, c=b.toString) and
// finding c requires knowing the type of b.
//
// The current solution is that as soon as a Phi node knows the type of one of
// it's inputs, it makes a new 'provisional type'. A provisional type means
// that we *think* it's of a particular type, but we're not sure. This
// provisional type will then be used for type calculation (expression nodes
// have functions that determines their output type based on their input types)
// as usual, but the output of these are also marked as provisional.
//
// The type inference runs on as usual, until a Phi node gets it's second (or
// third, etc) input. If the type is compatible with the first type it got,
// nothing changes. Otherwise it changes it's output type to unknown (or a
// loosened version of the original type - for example, going to a nullable
// type from a non-nullable type). Anything that used this type as an input
// may then have to change it's output, and so on causing type inference to
// repeat for whatever used the output of that Phi node.
//
// Eventually, all the Phi nodes have all their input types available and there
// aren't any conflicts (where a Phi node has inputs that are incompatible with
// their outputs). At this point we've found valid types for all the variables,
// which can be marked as no-longer-temporary.
//
// The main loop here is done with a worklist: whenever we update a variable's
// type (either because it changed or was set for the first time), all the
// variables that depend on it are added to a worklist. All the nodes on the
// worklist are updated, and a new worklist is used for any additions while
// the first worklist is being iterated through.
//
// In our implementation, we don't actually have separate provisional types,
// since while it's conceptually useful (and hence in the description above),
// it isn't actually required. We just notice that types that never change
// won't update their reverse-dependencies, and end when we have an empty
// worklist.
//
// Created by znix on 22/02/23.
//

#include "TypeInferencePass.h"
#include "ArenaAllocator.h"
#include "CompContext.h"
#include "Scope.h"
#include "VarType.h"

#include <set>

class TypeInferencePass::VarInfo : public BackendNodeData {
  public:
	// The variables whose types depend on this variable.
	std::vector<SSAVariable *> reverseDependencies;

	std::vector<SSAVariable *> dependencies;

	// The last time this variable was updated. This is used to avoid updating
	// a variable twice in a single iteration.
	int lastUpdateCycle = -1;

	// If this is not -1, then at least one of this variable's dependencies
	// aren't yet set (that is, for them setType==false). This points to the
	// first such variable where that's the case.
	// This is to avoid having to check the dependencies all the time.
	// This is set to 0 by default, so all the dependencies will have to be
	// checked.
	int16_t firstUnsetInput = 0;

	// True if the type variable has been set (in the case of Phi nodes, this
	// requires that all the inputs are also set).
	// This is used both to check when expressions can be safely evaluated, and
	// to assert that all nodes have been properly set at the end.
	bool setType = false;
};

TypeInferencePass::TypeInferencePass(ArenaAllocator *allocator) : m_allocator(allocator) {
	m_nullType = m_allocator->New<VarType>(VarType::NULL_TYPE);
	m_numType = m_allocator->New<VarType>(VarType::NUM);
	m_objectType = m_allocator->New<VarType>(VarType::OBJECT);

	m_nativeTypeObjString = VarType::SysClass(m_allocator, "ObjString");
	m_nativeTypeObjBool = VarType::SysClass(m_allocator, "ObjBool");
	m_nativeTypeObjRange = VarType::SysClass(m_allocator, "ObjRange");
	m_nativeTypeObjSystem = VarType::SysClass(m_allocator, "ObjSystem");
	m_nativeTypeObjFn = VarType::SysClass(m_allocator, "ObjFn");
	m_nativeTypeObjFibre = VarType::SysClass(m_allocator, "ObjFibre");
	m_nativeTypeObjClass = VarType::SysClass(m_allocator, "ObjClass");
	m_nativeTypeObjList = VarType::SysClass(m_allocator, "ObjList");
	m_nativeTypeObjMap = VarType::SysClass(m_allocator, "ObjMap");
}

void TypeInferencePass::Process(IRFn *fn) {
	ArenaAllocator tmpAlloc;
	m_tmpAllocator = &tmpAlloc;

	m_workList.clear(); // Should already be clear, but just in case.

	// This is a little inefficient doing big copies, but it's going to
	// be a big pain otherwise.
	std::vector<SSAVariable *> allVars;
	allVars.reserve(fn->temporaries.size() + fn->ssaVars.size());
	allVars.insert(allVars.begin(), fn->temporaries.begin(), fn->temporaries.end());
	allVars.insert(allVars.begin(), fn->ssaVars.begin(), fn->ssaVars.end());

	// Create the variable information.
	for (SSAVariable *var : allVars) {
		var->backendVarData = std::make_unique<VarInfo>();
	}

	// Set up the reverse dependency information, and find the set of variables
	// with no dependencies - those get added to the worklist.
	for (SSAVariable *var : allVars) {
		SetupVariable(var);
	}

	// Figure out the types, stopping when there's no more work left to do.
	std::vector<SSAVariable *> oldWorkList;
	int cycleNumber = 1;
	while (!m_workList.empty()) {
		// Pull out the current work list, adding all new nodes to an empty one.
		oldWorkList.clear();
		std::swap(oldWorkList, m_workList);

		for (SSAVariable *var : oldWorkList) {
			// Ignore variables without assignments.
			if (var->assignment == nullptr)
				continue;

			VarInfo *info = (VarInfo *)var->backendVarData.get();

			// Check if we've already updated this variable in this cycle, if
			// so don't try doing it again since it's very unlikely to have
			// changed in any way, and if it has then it'll be on the next
			// worklist.
			if (info->lastUpdateCycle == cycleNumber)
				continue;
			info->lastUpdateCycle = cycleNumber;

			// Check if all of this variable's dependencies are set. If they're
			// not, keep waiting. This is to avoid releasing a bunch of unknown
			// typed variables which then make everything else unknown, when we
			// could have got the actual types by waiting.
			// For example, updating a function when it's receiver is unknown
			// but one of it's arguments is known will obviously produce an
			// unknown result (or would, if we didn't have a check in
			// ExprLoad). This can then get pulled into a Phi node and make
			// everything unknown.
			if (info->firstUnsetInput != -1) {
				bool hitNotSet = false;
				while (true) {
					// Have we checked all the inputs?
					if (info->firstUnsetInput == (int16_t)info->dependencies.size()) {
						info->firstUnsetInput = -1;
						break;
					}

					VarInfo *dep = (VarInfo *)info->dependencies.at(info->firstUnsetInput)->backendVarData.get();
					if (dep->setType) {
						info->firstUnsetInput++;
						continue;
					}

					hitNotSet = true;
					break;
				}

				if (hitNotSet)
					continue;
			}

			// Compute the new type.
			VarType *newType = ProcessExpr(var->assignment->expr);

			// If the type has changed, update it's reverse dependencies. If
			// the type wasn't previously set, we also have to update them.
			// This is because if all the inputs to a variable are unknown,
			// then it would never get processed.
			if (newType == var->type && info->setType)
				continue;

			// Mark this type as having been computed.
			info->setType = true;

			var->type = newType;

			m_workList.insert(m_workList.end(), info->reverseDependencies.begin(), info->reverseDependencies.end());
		}

		cycleNumber++;
	}

	// Free the variable information.
	for (SSAVariable *var : allVars) {
		VarInfo *info = (VarInfo *)var->backendVarData.get();
		if (!info->setType) {
			fprintf(stderr, "Type inference: found variable '%s' that ended without having a type set!\n",
			    var->name.c_str());
			abort();
		}

		var->backendVarData.release();
	}

	m_tmpAllocator = nullptr;
}

void TypeInferencePass::SetupVariable(SSAVariable *var) {
	VarInfo *info = (VarInfo *)var->backendVarData.get();

	// Variables with no assignment? Ignore them, there's no way to
	// figure out what they are.
	if (var->assignment == nullptr) {
		// Mark this variable as having it's type set, to avoid triggering
		// an error later due to not having all the variable types set.
		info->setType = true;
		return;
	}

	IRExpr *expr = var->assignment->expr;

	bool hasDependencies = false;

	MarkDepFunc markDep = [&](SSAVariable *dependency) {
		// If variable A depends on variable B, and variable B doesn't have
		// it's assignment set, then ignore the dependency.
		// Since variable B won't ever be added to the work queue, this
		// variable would otherwise never be processed.
		if (dependency->assignment == nullptr)
			return;

		VarInfo *depInfo = (VarInfo *)dependency->backendVarData.get();
		depInfo->reverseDependencies.push_back(var);

		info->dependencies.push_back(dependency);

		hasDependencies = true;
	};

	// Phi nodes get special handling, since they can function with only a single
	// input dependency resolved.
	if (ExprPhi *phi = dynamic_cast<ExprPhi *>(expr)) {
		GetExprDepsPhi(phi, markDep);

		// This makes the node eligible to run with no inputs resolved,
		// however it won't actually be run until it's placed on the
		// worklist, which only happens when one of it's dependencies
		// is found.
		info->firstUnsetInput = -1;

		return;
	}

	// Mark all the variables used. Note that expressions with inputs of other
	// expressions will call GetExprDeps recursively, and eventually find all
	// the variable dependencies for that node.
	GetExprDeps(expr, markDep);

	// If this node doesn't have any dependencies, it can be updated
	// immediately - add it to the worklist.
	if (!hasDependencies) {
		m_workList.push_back(var);
	}
}

void TypeInferencePass::GetExprDeps(IRExpr *expr, const TypeInferencePass::MarkDepFunc &markDep) {
#define HANDLE_EXPR(type, func)                                                                                        \
	do {                                                                                                               \
		if (type *t = dynamic_cast<type *>(expr)) {                                                                    \
			return func(t, markDep);                                                                                   \
		}                                                                                                              \
	} while (0)

	HANDLE_EXPR(ExprConst, GetExprDepsConst);
	HANDLE_EXPR(ExprLoad, GetExprDepsLoad);
	HANDLE_EXPR(ExprLoadReceiver, GetExprDepsLoadReceiver);
	HANDLE_EXPR(ExprFuncCall, GetExprDepsFuncCall);
	HANDLE_EXPR(ExprSystemVar, GetExprDepsSystemVar);
	HANDLE_EXPR(ExprAllocateInstanceMemory, GetExprDepsAllocMem);
	HANDLE_EXPR(ExprClosure, GetExprDepsClosure);
	HANDLE_EXPR(ExprFieldLoad, GetExprDepsFieldLoad);
	// Phi nodes are intentionally not handled, they should be called directly by SetupVariable.
	// They're also not allowed to be contained inside another expression.
#undef HANDLE_EXPR

	fprintf(stderr, "Unknown expression type '%s' in type inference setup pass!\n", typeid(*expr).name());
	abort();
}

void TypeInferencePass::GetExprDepsConst(ExprConst *expr, const MarkDepFunc &markDep) {
	// Constants have no dependencies.
}
void TypeInferencePass::GetExprDepsLoad(ExprLoad *expr, const TypeInferencePass::MarkDepFunc &markDep) {
	SSAVariable *var = dynamic_cast<SSAVariable *>(expr->var);
	if (var) {
		markDep(var);
	}
	// Non-SSA variable loads don't have any dependencies, since we don't track types for locals or globals.
}
void TypeInferencePass::GetExprDepsLoadReceiver(ExprLoadReceiver *expr, const TypeInferencePass::MarkDepFunc &markDep) {
	// No dependencies.
}
void TypeInferencePass::GetExprDepsFuncCall(ExprFuncCall *expr, const TypeInferencePass::MarkDepFunc &markDep) {
	GetExprDeps(expr->receiver, markDep);
	for (IRExpr *arg : expr->args) {
		GetExprDeps(arg, markDep);
	}
}
void TypeInferencePass::GetExprDepsSystemVar(ExprSystemVar *expr, const MarkDepFunc &markDep) {
	// No dependencies.
}
void TypeInferencePass::GetExprDepsAllocMem(ExprAllocateInstanceMemory *expr, const MarkDepFunc &markDep) {
	// No dependencies. Don't bother with the foreign parameters, since it
	// won't influence the result type.
}
void TypeInferencePass::GetExprDepsClosure(ExprClosure *expr, const MarkDepFunc &markDep) {
	// No dependencies, the result is always a closure.
}
void TypeInferencePass::GetExprDepsFieldLoad(ExprFieldLoad *expr, const MarkDepFunc &markDep) {
	// No dependencies. Even when this is passed an argument when called in
	// a closure, we statically know what the 'this' type is.
}

void TypeInferencePass::GetExprDepsPhi(ExprPhi *expr, const TypeInferencePass::MarkDepFunc &markDependency) {
	for (SSAVariable *var : expr->inputs) {
		markDependency(var);
	}
}

VarType *TypeInferencePass::ProcessExpr(IRExpr *expr) {
#define HANDLE_EXPR(type, func)                                                                                        \
	do {                                                                                                               \
		if (type *t = dynamic_cast<type *>(expr)) {                                                                    \
			return func(t);                                                                                            \
		}                                                                                                              \
	} while (0)

	HANDLE_EXPR(ExprConst, ProcessExprConst);
	HANDLE_EXPR(ExprLoad, ProcessExprLoad);
	HANDLE_EXPR(ExprLoadReceiver, ProcessExprLoadReceiver);
	HANDLE_EXPR(ExprFuncCall, ProcessExprFuncCall);
	HANDLE_EXPR(ExprSystemVar, ProcessExprSystemVar);
	HANDLE_EXPR(ExprAllocateInstanceMemory, ProcessExprAllocMem);
	HANDLE_EXPR(ExprClosure, ProcessExprClosure);
	HANDLE_EXPR(ExprFieldLoad, ProcessExprFieldLoad);
	HANDLE_EXPR(ExprPhi, ProcessExprPhi);
#undef HANDLE_EXPR

	fprintf(stderr, "Unknown expression type '%s' in type inference pass!\n", typeid(*expr).name());
	abort();
}

VarType *TypeInferencePass::ProcessExprConst(ExprConst *expr) {
	switch (expr->value.type) {
	case CcValue::UNDEFINED:
		return nullptr;
	case CcValue::NULL_TYPE:
		return m_nullType;
	case CcValue::STRING:
		return m_nativeTypeObjString;
	case CcValue::BOOL:
		return m_nativeTypeObjBool;
	case CcValue::INT:
	case CcValue::NUM:
		return m_numType;
	default:
		fprintf(stderr, "Invalid constant value in type inference: %d\n", expr->value.type);
		abort();
	}
}

VarType *TypeInferencePass::ProcessExprLoad(ExprLoad *expr) {
	SSAVariable *var = dynamic_cast<SSAVariable *>(expr->var);

	// Non-SSA variables are always treated as unknown.
	if (!var) {
		return nullptr;
	}

	VarInfo *info = (VarInfo *)var->backendVarData.get();

	// This type should have been found by now, otherwise this expression
	// wouldn't have been evaluated.
	if (!info->setType) {
		fprintf(stderr, "Found non-set variable '%s' in ExprLoad!\n", var->name.c_str());
		abort();
	}

	// Use whatever type has been found. This might be wrong and later change,
	// that's fine. If it does, the expression type will be updated again.
	return var->type;
}

VarType *TypeInferencePass::ProcessExprLoadReceiver(ExprLoadReceiver *expr) {
	// TODO types for user-defined classes.
	return nullptr;
}

VarType *TypeInferencePass::ProcessExprSystemVar(ExprSystemVar *expr) {
	// TODO support static system variables
	return nullptr;
}
VarType *TypeInferencePass::ProcessExprAllocMem(ExprAllocateInstanceMemory *expr) {
	// This doesn't really matter, since it's only used in the small generated
	// allocation methods anyway.
	return nullptr;
}
VarType *TypeInferencePass::ProcessExprClosure(ExprClosure *expr) { return m_nativeTypeObjFn; }
VarType *TypeInferencePass::ProcessExprFieldLoad(ExprFieldLoad *expr) {
	// Fields aren't currently typed.
	return nullptr;
}

VarType *TypeInferencePass::ProcessExprFuncCall(ExprFuncCall *expr) {
	VarType *receiverType = ProcessExpr(expr->receiver);

	// If we don't know what the receiver is, we certainly can't figure
	// out what anything else is.
	if (receiverType == nullptr) {
		return nullptr;
	}

	std::string coreName;
	switch (receiverType->type) {
	case VarType::NULL_TYPE:
		coreName = "ObjNull";
		break;
	case VarType::NUM:
		coreName = "ObjNumClass";
		break;
	case VarType::OBJECT:
		// TODO object types
		return nullptr;
	case VarType::OBJECT_SYSTEM:
		coreName = receiverType->systemClassName;
		break;
	}

	FnInfo result;
	bool matched = GenGetCoreFunctionInfo(coreName, expr->signature->ToString(), result);
	if (!matched) {
		// Might be defined in wren_core, but we don't have any way to
		// access that yet.
		return nullptr;
	}

	// Check all the arguments. If the arguments are wrong then we'll still
	// get the same return type - we don't have method overloading - but it'll
	// call Fibre.abort at runtime.
	// In this case though, we need to make sure we don't make the call an
	// intrinsic, since that might replace the error message with undefined
	// behaviour.
	bool areArgsCorrect = true;
	for (int i = 0; i < (int)expr->args.size(); i++) {
		VarType *argType = ProcessExpr(expr->args.at(i));
		if (result.argTypes.at(i) != argType) {
			areArgsCorrect = false;
			break;
		}
	}

	// If this function call can be turned into an intrinsic in the backend, mark that.
	expr->intrinsic = areArgsCorrect ? result.intrinsic : ExprFuncCall::NONE;

	return result.returnType;
}

VarType *TypeInferencePass::ProcessExprPhi(ExprPhi *expr) {
	VarType *only = nullptr;
	int numFound = 0;

	// Unlike all other nodes, Phi nodes can produce an output even if some
	// of their dependencies are unset - we just use the only type we have,
	// which gets corrected later on as required.
	// This is really the key to the algorithm, see the block comment at the
	// top of the file.

	for (SSAVariable *var : expr->inputs) {
		VarInfo *info = (VarInfo *)var->backendVarData.get();
		if (!info->setType)
			continue;

		if (numFound == 0) {
			only = var->type;
		} else if (only != var->type) {
			// If we've found two different types, the result is always
			// unknown, without having to consider any of the other types.
			return nullptr;
		}

		numFound++;
	}

	if (numFound == 0) {
		fprintf(stderr, "Phi node evaluated without any set inputs!\n");
		abort();
	}

	// Return the only type we found.
	return only;
}
