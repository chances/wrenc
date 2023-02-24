//
// Created by znix on 22/02/23.
//

#pragma once

#include "IRNode.h"

#include <functional>

class ArenaAllocator;
class VarType;

class TypeInferencePass {
  public:
	TypeInferencePass(ArenaAllocator *allocator);

	void Process(IRFn *fn);

  private:
	class VarInfo;
	friend std::unique_ptr<VarInfo>;

	struct FnInfo {
		VarType *returnType = nullptr;
		bool isStatic = false;
		std::vector<VarType *> argTypes;
	};

	using MarkDepFunc = std::function<void(SSAVariable *)>;

	ArenaAllocator *m_allocator;

	// An arena allocator that's only active while a given function is being processed.
	ArenaAllocator *m_tmpAllocator = nullptr;

	// The variables that need to be updated on the next pass.
	std::vector<SSAVariable *> m_workList;

	// Basic important types
	VarType *m_nullType = nullptr;
	VarType *m_numType = nullptr;
	VarType *m_objectType = nullptr;

	// These are referenced by the generated C++, and need these exact names.
	VarType *m_nativeTypeObjString = nullptr;
	VarType *m_nativeTypeObjBool = nullptr;
	VarType *m_nativeTypeObjRange = nullptr;
	VarType *m_nativeTypeObjSystem = nullptr;
	VarType *m_nativeTypeObjFn = nullptr;
	VarType *m_nativeTypeObjFibre = nullptr;
	VarType *m_nativeTypeObjClass = nullptr;
	VarType *m_nativeTypeObjList = nullptr;
	VarType *m_nativeTypeObjMap = nullptr;

	void SetupVariable(SSAVariable *var);

	void GetExprDeps(IRExpr *expr, const MarkDepFunc &markDep);
	void GetExprDepsConst(ExprConst *expr, const MarkDepFunc &markDep);
	void GetExprDepsLoad(ExprLoad *expr, const MarkDepFunc &markDep);
	void GetExprDepsLoadReceiver(ExprLoadReceiver *expr, const MarkDepFunc &markDep);
	void GetExprDepsFuncCall(ExprFuncCall *expr, const MarkDepFunc &markDep);
	void GetExprDepsSystemVar(ExprSystemVar *expr, const MarkDepFunc &markDep);
	void GetExprDepsAllocMem(ExprAllocateInstanceMemory *expr, const MarkDepFunc &markDep);
	void GetExprDepsClosure(ExprClosure *expr, const MarkDepFunc &markDep);
	void GetExprDepsFieldLoad(ExprFieldLoad *expr, const MarkDepFunc &markDep);
	void GetExprDepsPhi(ExprPhi *expr, const MarkDepFunc &markDependency);

	VarType *ProcessExpr(IRExpr *expr);

	VarType *ProcessExprConst(ExprConst *expr);
	VarType *ProcessExprLoad(ExprLoad *expr);
	VarType *ProcessExprLoadReceiver(ExprLoadReceiver *expr);
	VarType *ProcessExprFuncCall(ExprFuncCall *expr);
	VarType *ProcessExprSystemVar(ExprSystemVar *expr);
	VarType *ProcessExprAllocMem(ExprAllocateInstanceMemory *expr);
	VarType *ProcessExprClosure(ExprClosure *expr);
	VarType *ProcessExprFieldLoad(ExprFieldLoad *expr);
	VarType *ProcessExprPhi(ExprPhi *expr);

	// This function is auto-generated!
	bool GenGetCoreFunctionInfo(const std::string &name, const std::string &signature, FnInfo &result);
};
