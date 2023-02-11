//
// Created by znix on 09/12/22.
//

#include "wrencc_config.h"

#ifdef USE_LLVM

// LLVM Backend
#include "WrenGCMetadataPrinter.h"
#include "WrenGCStrategy.h"

// Rest of the compiler
#include "ClassDescription.h"
#include "ClassInfo.h"
#include "CompContext.h"
#include "HashUtil.h"
#include "LLVMBackend.h"
#include "Scope.h"
#include "Utils.h"
#include "common.h"

#include <fmt/format.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/ModRef.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Transforms/Scalar/RewriteStatepointsForGC.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>

#include <fstream>
#include <map>

using CInt = llvm::ConstantInt;
namespace dwarf = llvm::dwarf;

// Wrap everything in a namespace to avoid any possibility of name collisions
namespace wren_llvm_backend {

class ScopedDebugGuard;

struct UpvaluePackDef {
	// All the variables bound to upvalues in the relevant closure
	std::vector<UpvalueVariable *> variables;

	// The positions of the variables in the upvalue pack, the inverse of variables
	std::unordered_map<UpvalueVariable *, int> variableIds;

	// The position of the pointer to the upvalue storage, for each upvalue storage
	// location used by this closure.
	std::unordered_map<StmtBeginUpvalues *, int> storageLocations;
};

struct VisitorContext {
	llvm::Value *receiver = nullptr;
	llvm::Value *fieldPointer = nullptr;

	/// The function's upvalue pack, used to reference upvalues from this closure's parent function.
	UpvaluePackDef *upvaluePack = nullptr;

	/// The array of upvalue value pointers.
	llvm::Value *upvaluePackPtr = nullptr;

	/// The BasicBlock that each label represents. These are created whenever they're first needed, but aren't
	/// added to the function until the StmtLabel is encountered in the AST.
	std::map<StmtLabel *, llvm::BasicBlock *> labelBlocks;

	llvm::Function *currentFunc = nullptr;
	IRFn *currentWrenFunc = nullptr;
	Module *currentModule = nullptr;
};

struct ExprRes {
	llvm::Value *value = nullptr;
};

struct StmtRes {};

class FnData : public BackendNodeData {
  public:
	llvm::Function *llvmFunc = nullptr;
	llvm::GlobalVariable *closureSpec = nullptr;
	std::unique_ptr<UpvaluePackDef> upvaluePackDef;
	std::vector<StmtBeginUpvalues *> beginUpvalueStatements;
};

class ClassData : public BackendNodeData {
  public:
	llvm::GlobalVariable *object = nullptr;
	llvm::GlobalVariable *fieldOffset = nullptr;
	llvm::GlobalVariable *classDataBlock = nullptr;

	std::unordered_map<Signature *, llvm::Function *> foreignStubs;
	std::unordered_map<Signature *, llvm::Function *> foreignStaticStubs;
};

class VarData : public BackendNodeData {
  public:
	/// For each local variable, stack memory is allocated for it (and later optimised away - we do this
	/// to avoid having to deal with SSA, and this is also how Clang does it) and the value for that
	/// stack address is stored here.
	///
	/// This is null for variables used by closures.
	llvm::Value *address = nullptr;

	/// The position in the storage block that contains this variable, or -1 if
	/// this variable isn't used by any upvalues.
	int closedAddressPosition = -1;
};

/// Stores information about a storage block (the thing that contains local variables
/// that are used as upvalues).
struct BeginUpvaluesData : public BackendNodeData {
	/// An alloca-ed pointer that stores the address of the storage block this
	/// variable belongs to.
	llvm::Value *packPtr = nullptr;

	/// The variables stored in the block, in the same order as they are arranged in memory.
	/// This should have the same contents (but possibly in a different order) as
	/// StmtBeginUpvalues.variables.
	std::vector<LocalVariable *> contents;

	/// The function that contains this block - useful when working with upvalues.
	IRFn *function = nullptr;

	/// Set to true if this block was visited early so the arguments could be loaded safely.
	bool createdEarly = false;
};

class LLVMBackendImpl : public LLVMBackend {
  public:
	LLVMBackendImpl();

	CompilationResult Generate(Module *mod, const CompilationOptions *options) override;

  private:
	/// The number of 64-bit values at the start of any upvalue storage block
	/// that are reserved for things like the reference counter.
	static constexpr int UPVALUE_STORAGE_OFFSET = 1;

	llvm::Function *GenerateFunc(IRFn *func, Module *mod);
	void GenerateInitialiser(Module *mod);
	void GenerateForeignStubs(Module *mod);
	void SetupDebugInfo(Module *mod);

	llvm::Constant *GetStringConst(const std::string &str);
	llvm::Value *GetManagedStringValue(const std::string &str);
	llvm::GlobalVariable *GetGlobalVariable(IRGlobalDecl *global);
	llvm::Value *GetLocalPointer(VisitorContext *ctx, LocalVariable *local);
	llvm::Value *GetUpvaluePointer(VisitorContext *ctx, UpvalueVariable *upvalue);
	llvm::Value *GetVariablePointer(VisitorContext *ctx, VarDecl *var);
	llvm::BasicBlock *GetLabelBlock(VisitorContext *ctx, StmtLabel *label);
	llvm::Value *GetObjectFieldPointer(VisitorContext *ctx, VarDecl *thisVar);

	/// Process a string literal, to make it suitable for using as a name in the IR
	static std::string FilterStringLiteral(const std::string &literal);

	ExprRes VisitExpr(VisitorContext *ctx, IRExpr *expr);
	StmtRes VisitStmt(VisitorContext *ctx, IRStmt *expr);
	void WriteNodeDebugInfo(IRNode *node);

	ExprRes VisitExprConst(VisitorContext *ctx, ExprConst *node);
	ExprRes VisitExprLoad(VisitorContext *ctx, ExprLoad *node);
	ExprRes VisitExprFieldLoad(VisitorContext *ctx, ExprFieldLoad *node);
	ExprRes VisitExprFuncCall(VisitorContext *ctx, ExprFuncCall *node);
	ExprRes VisitExprClosure(VisitorContext *ctx, ExprClosure *node);
	ExprRes VisitExprLoadReceiver(VisitorContext *ctx, ExprLoadReceiver *node);
	ExprRes VisitExprRunStatements(VisitorContext *ctx, ExprRunStatements *node);
	ExprRes VisitExprAllocateInstanceMemory(VisitorContext *ctx, ExprAllocateInstanceMemory *node);
	ExprRes VisitExprSystemVar(VisitorContext *ctx, ExprSystemVar *node);
	ExprRes VisitExprGetClassVar(VisitorContext *ctx, ExprGetClassVar *node);

	StmtRes VisitStmtAssign(VisitorContext *ctx, StmtAssign *node);
	StmtRes VisitStmtFieldAssign(VisitorContext *ctx, StmtFieldAssign *node);
	StmtRes VisitStmtEvalAndIgnore(VisitorContext *ctx, StmtEvalAndIgnore *node);
	StmtRes VisitBlock(VisitorContext *ctx, StmtBlock *node);
	StmtRes VisitStmtLabel(VisitorContext *ctx, StmtLabel *node);
	StmtRes VisitStmtJump(VisitorContext *ctx, StmtJump *node);
	StmtRes VisitStmtReturn(VisitorContext *ctx, StmtReturn *node);
	StmtRes VisitStmtLoadModule(VisitorContext *ctx, StmtLoadModule *node);
	StmtRes VisitStmtBeginUpvalues(VisitorContext *ctx, StmtBeginUpvalues *node);
	StmtRes VisitStmtRelocateUpvalues(VisitorContext *ctx, StmtRelocateUpvalues *node);
	StmtRes VisitStmtDefineClass(VisitorContext *ctx, StmtDefineClass *node);

	llvm::LLVMContext m_context;
	llvm::IRBuilder<> m_builder;
	llvm::Module m_module;

	std::unique_ptr<llvm::DIBuilder> m_debugInfo;
	llvm::DISubprogram *m_dbgSubProgramme = nullptr;
	IRNode *m_dbgCurrentNode = nullptr;
	bool m_dbgEnable = false;

	llvm::Function *m_initFunc = nullptr;
	llvm::Function *m_getGlobals = nullptr;

	llvm::FunctionCallee m_virtualMethodLookup;
	llvm::FunctionCallee m_superMethodLookup;
	llvm::FunctionCallee m_createClosure;
	llvm::FunctionCallee m_allocUpvalueStorage;
	llvm::FunctionCallee m_unrefUpvalueStorage;
	llvm::FunctionCallee m_getUpvaluePack;
	llvm::FunctionCallee m_getNextClosure;
	llvm::FunctionCallee m_allocObject;
	llvm::FunctionCallee m_initClass;
	llvm::FunctionCallee m_getClassFieldOffset;
	llvm::FunctionCallee m_registerSignatureTable;
	llvm::FunctionCallee m_importModule;
	llvm::FunctionCallee m_getModuleVariable;
	llvm::FunctionCallee m_callForeignMethod;
	llvm::FunctionCallee m_dummyPtrBitcast;

	// Intrinsics
	llvm::FunctionCallee m_ptrMask;

	llvm::PointerType *m_pointerType = nullptr;
	llvm::Type *m_signatureType = nullptr;
	llvm::PointerType *m_valueType = nullptr;
	llvm::IntegerType *m_int8Type = nullptr;
	llvm::IntegerType *m_int32Type = nullptr;
	llvm::IntegerType *m_int64Type = nullptr;
	llvm::Type *m_voidType = nullptr;

	llvm::DIBasicType *m_dbgValueType = nullptr;
	llvm::DIBasicType *m_dbgUpvaluePackType = nullptr;
	llvm::DIFile *m_dbgFile = nullptr;
	llvm::DICompileUnit *m_dbgCompileUnit = nullptr;

	llvm::Constant *m_nullValue = nullptr; // This must NOT be used in function bodies - see m_dummyPtrBitcast
	llvm::Constant *m_nullPointer = nullptr;

	std::map<std::string, llvm::GlobalVariable *> m_systemVars;
	std::map<std::string, llvm::GlobalVariable *> m_stringConstants;
	std::map<std::string, llvm::GlobalVariable *> m_managedStrings;
	std::map<IRGlobalDecl *, llvm::GlobalVariable *> m_globalVariables;

	llvm::GlobalVariable *m_valueFalsePtr = nullptr, *m_valueTruePtr = nullptr;

	// A set of all the system variables used in the code. Any other system variables will be removed.
	std::set<llvm::GlobalVariable *> m_usedSystemVars;

	// All the signatures we've ever used, for the signatures table
	std::set<std::string> m_signatures;

	// Should statepoints (for GC use) be generated?
	bool m_enableStatepoints = true;

	friend ScopedDebugGuard;
};

/// Sets the debug position to a given node while this object is in scope, then returns it afterwards
class ScopedDebugGuard {
  public:
	ScopedDebugGuard(LLVMBackendImpl *backend, IRNode *node);
	~ScopedDebugGuard();

  private:
	LLVMBackendImpl *m_backend = nullptr;
	IRNode *m_previous = nullptr;
};

LLVMBackendImpl::LLVMBackendImpl() : m_builder(m_context), m_module("myModule", m_context) {
	m_valueType = llvm::PointerType::get(m_context, WrenGCStrategy::VALUE_ADDR_SPACE);
	m_signatureType = llvm::Type::getInt64Ty(m_context);
	m_pointerType = llvm::PointerType::get(m_context, 0);
	m_int8Type = llvm::Type::getInt8Ty(m_context);
	m_int32Type = llvm::Type::getInt32Ty(m_context);
	m_int64Type = llvm::Type::getInt64Ty(m_context);
	m_voidType = llvm::Type::getVoidTy(m_context);

	m_nullValue = llvm::ConstantExpr::getIntToPtr(CInt::get(m_int64Type, encode_object(nullptr)), m_valueType);
	m_nullPointer = llvm::ConstantPointerNull::get(m_pointerType);

	// Mark for functions during which GC stack scanning won't occur - generally if something allocates memory then
	// it probably shouldn't be marked with this.
	llvm::AttributeList gcLeafFunc =
	    llvm::AttributeList::get(m_context, llvm::AttributeList::FunctionIndex, {"gc-leaf-function"});

	std::vector<llvm::Type *> fnLookupArgs = {m_valueType, m_signatureType};
	llvm::FunctionType *fnLookupType = llvm::FunctionType::get(m_pointerType, fnLookupArgs, false);
	m_virtualMethodLookup = m_module.getOrInsertFunction("wren_virtual_method_lookup", fnLookupType, gcLeafFunc);

	llvm::FunctionType *superLookupType =
	    llvm::FunctionType::get(m_pointerType, {m_valueType, m_valueType, m_int64Type, m_int8Type}, false);
	m_superMethodLookup = m_module.getOrInsertFunction("wren_super_method_lookup", superLookupType, gcLeafFunc);

	std::vector<llvm::Type *> newClosureArgs = {m_pointerType, m_pointerType, m_pointerType, m_pointerType};
	llvm::FunctionType *newClosureTypes = llvm::FunctionType::get(m_valueType, newClosureArgs, false);
	m_createClosure = m_module.getOrInsertFunction("wren_create_closure", newClosureTypes);

	// This allocates memory, but it's a GC leaf function anyway since the closure packs will be stored separately
	llvm::FunctionType *allocUpvalueStorageType = llvm::FunctionType::get(m_pointerType, {m_int32Type}, false);
	m_allocUpvalueStorage =
	    m_module.getOrInsertFunction("wren_alloc_upvalue_storage", allocUpvalueStorageType, gcLeafFunc);

	llvm::FunctionType *unrefUpvalueStorageType = llvm::FunctionType::get(m_voidType, {m_pointerType}, false);
	m_unrefUpvalueStorage =
	    m_module.getOrInsertFunction("wren_unref_upvalue_storage", unrefUpvalueStorageType, gcLeafFunc);

	llvm::FunctionType *getUpvaluePackType = llvm::FunctionType::get(m_pointerType, {m_valueType}, false);
	m_getUpvaluePack = m_module.getOrInsertFunction("wren_get_closure_upvalue_pack", getUpvaluePackType, gcLeafFunc);

	llvm::FunctionType *allocObjectType = llvm::FunctionType::get(m_valueType, {m_valueType}, false);
	m_allocObject = m_module.getOrInsertFunction("wren_alloc_obj", allocObjectType);

	llvm::FunctionType *initClassType =
	    llvm::FunctionType::get(m_valueType, {m_pointerType, m_pointerType, m_pointerType, m_valueType}, false);
	m_initClass = m_module.getOrInsertFunction("wren_init_class", initClassType);

	llvm::FunctionType *getFieldOffsetType = llvm::FunctionType::get(m_int32Type, {m_valueType}, false);
	m_getClassFieldOffset = m_module.getOrInsertFunction("wren_class_get_field_offset", getFieldOffsetType);

	llvm::FunctionType *registerSigTableType = llvm::FunctionType::get(m_voidType, {m_pointerType}, false);
	m_registerSignatureTable = m_module.getOrInsertFunction("wren_register_signatures_table", registerSigTableType);

	llvm::FunctionType *importModuleType =
	    llvm::FunctionType::get(m_pointerType, {m_pointerType, m_pointerType}, false);
	m_importModule = m_module.getOrInsertFunction("wren_import_module", importModuleType);

	llvm::FunctionType *getModuleVariableType =
	    llvm::FunctionType::get(m_valueType, {m_pointerType, m_pointerType}, false);
	m_getModuleVariable = m_module.getOrInsertFunction("wren_get_module_global", getModuleVariableType);

	llvm::FunctionType *callForeignMethodType = llvm::FunctionType::get(m_valueType,
	    {m_pointerType, m_pointerType, m_int32Type, m_valueType, m_pointerType}, false);
	m_callForeignMethod = m_module.getOrInsertFunction("wren_call_foreign_method", callForeignMethodType);

	// RS4GC really doesn't like inttoptr bitcasts, and they make it think one value is derived from another.
	// See https://llvm.org/docs/Statepoints.html#mixing-references-and-raw-pointers
	// To solve this, we make a dummy function that's not implemented anywhere that takes an i64 and produces
	// a value (which itself is a pointer in a different address space). We'll then replace it with a real
	// bitcast after running RS4GC.
	llvm::AttributeSet castAttrs = llvm::AttributeSet::get(m_context,
	    {
	        llvm::Attribute::getWithMemoryEffects(m_context, llvm::MemoryEffects::none()),
	        llvm::Attribute::get(m_context, llvm::Attribute::NoUnwind),
	        llvm::Attribute::get(m_context, llvm::Attribute::WillReturn),
	        llvm::Attribute::get(m_context, "gc-leaf-function"),
	    });
	llvm::AttributeList castAttrList =
	    llvm::AttributeList::get(m_context, llvm::AttributeList::FunctionIndex, castAttrs);

	llvm::FunctionType *dummyPtrCastType = llvm::FunctionType::get(m_valueType, {m_int64Type}, false);
	m_dummyPtrBitcast = m_module.getOrInsertFunction("!!dummy_ptr_bitcast", dummyPtrCastType, castAttrList);

	// Intrinsics lookup
	m_ptrMask = llvm::Intrinsic::getDeclaration(&m_module, llvm::Intrinsic::ptrmask, {m_valueType, m_int64Type});
}

CompilationResult LLVMBackendImpl::Generate(Module *mod, const CompilationOptions *options) {
	// TODO create the module with the source file name
	m_getGlobals = nullptr;

	if (mod->sourceFilePath) {
		// Assuming it wants just the filename, not the full path
		std::vector<std::string> parts = utils::stringSplit(mod->sourceFilePath.value(), "/");
		m_module.setSourceFileName(parts.back());
	}

	m_dbgEnable = options->includeDebugInfo;
	m_enableStatepoints = options->enableGCSupport;
	SetupDebugInfo(mod);

	// Create all the system variables with the correct linkage
	for (const auto &entry : ExprSystemVar::SYSTEM_VAR_NAMES) {
		std::string name = "wren_sys_var_" + entry.first;
		m_systemVars[entry.first] = new llvm::GlobalVariable(m_module, m_valueType, false,
		    llvm::GlobalVariable::InternalLinkage, m_nullValue, name);
	}

	m_valueTruePtr = new llvm::GlobalVariable(m_module, m_valueType, false, llvm::GlobalVariable::InternalLinkage,
	    m_nullValue, "gbl_trueValue");
	m_valueFalsePtr = new llvm::GlobalVariable(m_module, m_valueType, false, llvm::GlobalVariable::InternalLinkage,
	    m_nullValue, "gbl_falseValue");

	llvm::FunctionType *initFuncType = llvm::FunctionType::get(llvm::Type::getVoidTy(m_context), false);
	m_initFunc = llvm::Function::Create(initFuncType, llvm::Function::PrivateLinkage, "module_init", &m_module);

	// Create our function data objects
	for (IRFn *func : mod->GetFunctions()) {
		FnData *fnData = new FnData;
		func->backendData = std::unique_ptr<BackendNodeData>(fnData);

		// Create all the BeginUpvalueData objects, so we can later use them
		// during upvalue handling without worrying about whether they've
		// been generated yet or not.
		// Note we don't need to search the temporary variables, since they're
		// not used by upvalues.
		std::set<StmtBeginUpvalues *> allBeginUpvalues;
		for (LocalVariable *var : func->locals) {
			if (var->beginUpvalues) {
				allBeginUpvalues.insert(var->beginUpvalues);
			}
		}
		for (StmtBeginUpvalues *beginUpvalues : allBeginUpvalues) {
			// This variable is accessed by closures, so it gets stored in the array of closable variables.
			// First, get or create the storage pack for the block containing this local.
			BeginUpvaluesData *data = new BeginUpvaluesData;
			beginUpvalues->backendData = std::unique_ptr<BackendNodeData>(data);
			fnData->beginUpvalueStatements.push_back(beginUpvalues);

			data->function = func;
		}
	}

	for (IRFn *func : mod->GetClosures()) {
		// Make a global variable for the ClosureSpec
		FnData *fnData = func->GetBackendData<FnData>();
		fnData->closureSpec = new llvm::GlobalVariable(m_module, m_pointerType, false,
		    llvm::GlobalVariable::InternalLinkage, m_nullPointer, "spec_" + func->debugName);

		// Make the upvalue pack for each function that needs them
		std::unique_ptr<UpvaluePackDef> pack = std::make_unique<UpvaluePackDef>();

		for (const auto &entry : func->upvalues) {
			// Assign an increasing series of IDs for each variable in an arbitrary order
			pack->variables.push_back(entry.second);
			pack->variableIds[entry.second] = pack->variables.size() - 1; // -1 to get the index of the last entry
		}

		// Generate the storage pointers - these are used for reference-counting
		// all the storage objects that store the upvalue Values.
		for (const auto &entry : func->upvalues) {
			// Find the local variable this upvalue ultimately points to, walking through an arbitrary
			// number of nested upvalues first.
			LocalVariable *target = entry.second->GetFinalTarget();

			if (!pack->storageLocations.contains(target->beginUpvalues)) {
				int nextPosition = pack->storageLocations.size() + pack->variables.size();
				pack->storageLocations[target->beginUpvalues] = nextPosition;
			}
		}

		// Note we always have to register an upvalue pack definition, even if it's empty - it's required for closures.
		fnData->upvaluePackDef = std::move(pack);
	}

	// Add pointers to the ObjClass instances for all the classes we've declared
	for (IRClass *cls : mod->GetClasses()) {
		ClassData *classData = new ClassData;
		cls->backendData = std::unique_ptr<BackendNodeData>(classData);

		// Create a constant containing information about all our functions, annotations, etc.
		// The way we'll do this is pretty horrible: create a global variable now to point to another
		// global variable we create later.
		// This really isn't very nice, but for both functions and these global tables we generate
		// the type at the same time as the body, and both reference each other so there's
		// not really a clean way of removing this layer of indirection.
		classData->classDataBlock = new llvm::GlobalVariable(m_module, m_pointerType, true,
		    llvm::GlobalVariable::InternalLinkage, nullptr, "class_data_ptr_" + cls->info->name);

		// Some system classes are defined in C++, and should only appear when compiling wren_core.
		// For these, the class object is created by the runtime.
		if (cls->info->IsCppSystemClass())
			continue;

		classData->object = new llvm::GlobalVariable(m_module, m_valueType, false,
		    llvm::GlobalVariable::InternalLinkage, m_nullValue, "class_obj_" + cls->info->name);

		// Add fields to store the offset of the member fields in each class. This is required since we
		// can currently have classes extending from classes in another file, and they don't know how
		// many fields said classes have. Thus this field will get loaded at startup as a byte offset and
		// we have to add it to the object pointer to get the fields area.
		classData->fieldOffset = new llvm::GlobalVariable(m_module, m_int32Type, false,
		    llvm::GlobalVariable::InternalLinkage, CInt::get(m_int32Type, 0), "class_field_offset_" + cls->info->name);
	}

	// Create the get-global-table function early, as we pass a pointer to it for class initialisation.
	{
		std::string getGlobalName = mod->Name() + "_get_globals";
		llvm::FunctionType *getGblFuncType = llvm::FunctionType::get(m_pointerType, false);
		m_getGlobals =
		    llvm::Function::Create(getGblFuncType, llvm::Function::ExternalLinkage, getGlobalName, &m_module);
	}

	for (IRFn *func : mod->GetFunctions()) {
		FnData *data = func->GetBackendData<FnData>();
		data->llvmFunc = GenerateFunc(func, mod);
	}

	// Create 'stub' methods for each foreign function, which passes control off to the runtime.
	GenerateForeignStubs(mod);

	// Generate the initialiser last, when we know all the string constants etc
	GenerateInitialiser(mod);

	// Identify this module as containing the main function
	if (defineStandaloneMainModule) {
		// Emit a pointer to the main module function. This is picked up by the stub the programme gets linked to.
		// This stub (in rtsrc/standalone_main_stub.cpp) uses the OS's standard crti/crtn and similar objects to
		// make a working executable, and it'll load this pointer when we link this object to it.
		// Also, put it in .data not .rodata since it contains a relocation.
		assert(m_getGlobals);
		new llvm::GlobalVariable(m_module, m_pointerType, true, llvm::GlobalVariable::ExternalLinkage, m_getGlobals,
		    "wrenStandaloneMainModule");
	}

	// We're done writing functions at this point, so finalise the debug information - this is also the latest
	// we can wait since we have to do it before dumping/optimising/etc the IR.
	if (m_dbgEnable) {
		m_debugInfo->finalize();
	}
	m_debugInfo.reset();

	// Run the middle-end passes - optimisation and any lowering we need
	// TODO make these (except for RS4GC) optional
	llvm::OptimizationLevel level;
	switch (options->optimisationLevel) {
	case WrenOptimisationLevel::NONE:
		level = llvm::OptimizationLevel::O0;
		break;
	case WrenOptimisationLevel::FAST:
		level = llvm::OptimizationLevel::O3;
		break;
	}

	{
		// See https://llvm.org/docs/NewPassManager.html
		llvm::PassBuilder pb;

		// Register all the basic analyses with the managers.
		llvm::LoopAnalysisManager lam;
		llvm::FunctionAnalysisManager fam;
		llvm::CGSCCAnalysisManager cgam;
		llvm::ModuleAnalysisManager mam;

		pb.registerModuleAnalyses(mam);
		pb.registerCGSCCAnalyses(cgam);
		pb.registerFunctionAnalyses(fam);
		pb.registerLoopAnalyses(lam);
		pb.crossRegisterProxies(lam, fam, cgam, mam);

		// If we're going to run RS4GC, we ALWAYS ALWAYS have to run mem2reg before it. RS4GC ignores
		// values intentionally placed on the stack with alloca, so if there's anything that doesn't
		// get moved to registers we can end up freeing something that's still reachable!
		// Fortunately, mem2reg is guaranteed to pick up all our local variable allocations (for the specific
		// logic see llvm::isAllocaPromotable, but in short if it's just non-volatile loads/stores and
		// the pointer to the alloca-d region is never passed to anything else, then it'll always be transformed).
		// Do this first, before anything else, as moving our local variables from the heap to registers is a
		// pretty essential pass for basically everything else.
		pb.registerPipelineStartEPCallback([](llvm::ModulePassManager &mpm, llvm::OptimizationLevel level) {
			mpm.addPass(llvm::createModuleToFunctionPassAdaptor(llvm::PromotePass()));
		});

		llvm::ModulePassManager mpm;
		if (level == llvm::OptimizationLevel::O0) {
			mpm = pb.buildO0DefaultPipeline(level);
		} else {
			mpm = pb.buildPerModuleDefaultPipeline(level);
		}

		// Add the RS4GC pass - without this, we won't have any statepoints
		// Run this regardless of if statepoints are enabled, since it won't do anything if
		// a function doesn't have a GC strategy set.
		mpm.addPass(llvm::RewriteStatepointsForGC());

		mpm.run(m_module, mam);
	}

	// Replace all usages of our bitcast function with a real function, now we're done with the RS4GC pass.
	for (llvm::User *user : m_dummyPtrBitcast.getCallee()->users()) {
		llvm::CallInst *call = llvm::dyn_cast<llvm::CallInst>(user);
		if (!call) {
			fmt::print(stderr, "dummy bitcast used by non-call user: {}\n", user->getName());
			user->print(llvm::errs());
			abort();
		}

		llvm::Value *arg = call->getArgOperand(0);
		llvm::Constant *constArg = llvm::dyn_cast<llvm::Constant>(arg);

		if (constArg) {
			llvm::Constant *asPtr = llvm::ConstantExpr::getIntToPtr(constArg, m_valueType);

			llvm::BasicBlock::iterator iter(call);
			llvm::ReplaceInstWithValue(iter, asPtr);
		} else {
			// TODO if we need to support non-constant ints, use BitCastInst and ReplaceInstWithInst
			fmt::print(stderr, "dummy bitcast used with non-const arg (not implemented): {}\n", user->getName());
			user->print(llvm::errs());
			abort();
		}
	}

	// Print out the LLVM IR for debugging
	const char *dumpIrStr = getenv("DUMP_LLVM_IR");
	if (dumpIrStr && dumpIrStr == std::string("1")) {
		m_module.print(llvm::outs(), nullptr);

		std::error_code ec;
		llvm::raw_fd_ostream output("/tmp/wren_llvm_ir.ll", ec, llvm::sys::fs::OF_None);
		m_module.print(output, nullptr);
	}

	// Verify the IR, to make sure we haven't done something strange
	if (llvm::verifyModule(m_module, &llvm::errs())) {
		fprintf(stderr, "LLVM IR Validation failed!\n");
		exit(1);
	}

	// Compile it - FIXME is this going to constantly re-initialise everything?
	llvm::InitializeAllTargetInfos();
	llvm::InitializeAllTargets();
	llvm::InitializeAllTargetMCs();
	llvm::InitializeAllAsmParsers();
	llvm::InitializeAllAsmPrinters();

	// Compile for the default target, TODO this should be configurable
	std::string targetTriple = llvm::sys::getDefaultTargetTriple();

	std::string lookupError;
	const llvm::Target *target = llvm::TargetRegistry::lookupTarget(targetTriple, lookupError);
	if (!target) {
		fprintf(stderr, "Failed to lookup target '%s'\n", targetTriple.c_str());
		exit(1);
	}

	// CPU features to use - eg SSE, AVX, NEON
	std::string cpu = "generic";
	std::string features = "";

	llvm::TargetOptions opt;
	std::optional<llvm::Reloc::Model> relocModel = llvm::Reloc::PIC_;
	llvm::TargetMachine *targetMachine = target->createTargetMachine(targetTriple, cpu, features, opt, relocModel);

	m_module.setDataLayout(targetMachine->createDataLayout());
	m_module.setTargetTriple(targetTriple);

	// Actually generate the code
	std::string filename = "wren-output-!!!!!!!.o";
	filename = utils::buildTempFilename(filename);

	std::error_code ec;
	llvm::raw_fd_ostream dest(filename, ec, llvm::sys::fs::OF_None);

	if (ec) {
		std::string msg = ec.message();
		fprintf(stderr, "Could not open file: %s", msg.c_str());
		exit(1);
	}

	// TODO switch to the new PassManager
	llvm::legacy::PassManager pass;
	llvm::CodeGenFileType fileType = options->forceAssemblyOutput ? llvm::CGFT_AssemblyFile : llvm::CGFT_ObjectFile;

	if (targetMachine->addPassesToEmitFile(pass, dest, nullptr, fileType)) {
		fprintf(stderr, "TargetMachine can't emit a file of this type");
		exit(1);
	}

	pass.run(m_module);

	// Flush what we can now, a bit more might come out when the pass manager's destructor runs
	dest.flush();

	return CompilationResult{
	    .successful = true,
	    .tempFilename = filename,
	    .format = options->forceAssemblyOutput ? CompilationResult::ASSEMBLY : CompilationResult::OBJECT,
	};
}

llvm::Function *LLVMBackendImpl::GenerateFunc(IRFn *func, Module *mod) {
	FnData *fnData = func->GetBackendData<FnData>();

	// Only take an upvalue pack argument if we actually need it
	bool takesUpvaluePack = fnData->upvaluePackDef && !fnData->upvaluePackDef->variables.empty();

	// Set up the function arguments - there's two arrays, one for the LLVM types and one for the DWARF types
	std::vector<llvm::Type *> funcArgs;
	std::vector<llvm::Metadata *> debugTypeArgs;
	if (func->enclosingClass) {
		// The receiver ('this') value.
		funcArgs.push_back(m_valueType);
		debugTypeArgs.push_back(m_dbgValueType);
	}
	if (takesUpvaluePack) {
		// If this function uses upvalues, they're passed as an argument
		funcArgs.push_back(m_pointerType);
		debugTypeArgs.push_back(m_dbgUpvaluePackType);
	}

	// The 'regular' arguments, that the user would see
	funcArgs.insert(funcArgs.end(), func->parameters.size(), m_valueType);
	debugTypeArgs.insert(debugTypeArgs.end(), func->parameters.size(), m_dbgValueType);

	llvm::FunctionType *ft = llvm::FunctionType::get(m_valueType, funcArgs, false);

	llvm::Function *function = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, func->debugName, &m_module);
	if (m_enableStatepoints) {
		// Turning on the GC significantly alters the generated code from the RS4GC (RewriteStatepointsForGC) LLVM
		// pass. This spills all the Value-s to the stack before most function calls, and loads them back afterwards.
		// This allows the GC to relocate objects and update the values without the programme noticing, but does
		// have a performance compared to not doing so.
		// TODO make an annotation to disable this for a function - but be clear that will prevent the GC from running
		//  if that function is on the stack.
		function->setGC(WrenGCStrategy::GetStrategyName());
	}
	llvm::BasicBlock *bb = llvm::BasicBlock::Create(m_context, "entry", function);
	m_builder.SetInsertPoint(bb);

	if (func == mod->GetMainFunction()) {
		// Call the initialiser, which we'll generate later
		m_builder.CreateCall(m_initFunc->getFunctionType(), m_initFunc);
	}

	// Set up the function's debug information
	if (m_dbgEnable) {
		llvm::DISubroutineType *dbgSubType =
		    m_debugInfo->createSubroutineType(m_debugInfo->getOrCreateTypeArray(debugTypeArgs));
		int lineNum = func->debugInfo.lineNumber;
		m_dbgSubProgramme = m_debugInfo->createFunction(m_dbgCompileUnit, func->debugName, llvm::StringRef(), m_dbgFile,
		    lineNum, dbgSubType, lineNum, llvm::DINode::FlagPrototyped, llvm::DISubprogram::SPFlagDefinition);
		function->setSubprogram(m_dbgSubProgramme);
	}

	// This should be null here, just make sure it is - this is done by WriteNodeDebugInfo anyway, but
	// putting it here for clarity.
	m_dbgCurrentNode = nullptr;

	// From https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/LangImpl09.html
	// Unset the location for the prologue emission (leading instructions with no
	// location in a function are considered part of the prologue and the debugger
	// will run past them when breaking on a function)
	WriteNodeDebugInfo(nullptr);

	VisitorContext ctx = {};
	ctx.currentFunc = function;
	ctx.currentWrenFunc = func;
	ctx.currentModule = mod;

	for (LocalVariable *local : func->locals) {
		VarData *varData = new VarData;
		local->backendVarData = std::unique_ptr<BackendNodeData>(varData);

		if (local->upvalues.empty()) {
			// Normal local variable
			varData->address = m_builder.CreateAlloca(m_valueType, nullptr, local->Name());
			continue;
		}

		// This variable is accessed by closures, so it gets stored in the array of closable variables.
		// Reserve an index in the storage pack for this variable.
		BeginUpvaluesData *beginUpvalues = local->beginUpvalues->GetBackendData<BeginUpvaluesData>();
		varData->closedAddressPosition = (int)beginUpvalues->contents.size();
		beginUpvalues->contents.push_back(local);
	}
	for (LocalVariable *local : func->temporaries) {
		VarData *varData = new VarData;
		local->backendVarData = std::unique_ptr<BackendNodeData>(varData);
		varData->address = m_builder.CreateAlloca(m_valueType, nullptr, local->Name());
	}

	// Create the memory to store the pointers to the storage blocks
	for (StmtBeginUpvalues *beginUpvalues : fnData->beginUpvalueStatements) {
		BeginUpvaluesData *data = beginUpvalues->GetBackendData<BeginUpvaluesData>();
		data->packPtr = m_builder.CreateAlloca(m_pointerType, nullptr, "storage_blk");
	}

	// Load the upvalue pack
	int nextArg = 0;
	if (func->enclosingClass) {
		ctx.receiver = function->getArg(nextArg++);
		ctx.receiver->setName("this");

		// Find a pointer to this object, since the 'this' argument is a value.
		// We'll just assume it's always a pointer, since there's no valid reason why it could be a number.
		// (note this is still an i64, we'll convert it to a pointer later)
		llvm::Value *intThisVal = m_builder.CreatePtrToInt(ctx.receiver, m_int64Type);
		llvm::Value *thisPtr = m_builder.CreateAnd(intThisVal, CInt::get(m_int64Type, CONTENT_MASK));

		// Also find the pointer to our field block
		// We don't know the position of the class's fields at compile time, they're stored in a variable that's
		// loaded during class initialisation. Use this as an offset into our object to find where the fields start.
		// We only need this if we access fields, but that's quite common and LLVM should optimise it out if
		// it's not needed

		ClassData *cd = func->enclosingClass->GetBackendData<ClassData>();
		if (cd->fieldOffset) {
			llvm::Value *fieldStartOffset = m_builder.CreateLoad(m_int32Type, cd->fieldOffset, "this_field_offset");
			llvm::Value *wideStartOffset =
			    m_builder.CreateIntCast(fieldStartOffset, m_int64Type, false, "this_field_offset_64");

			llvm::Value *fieldPtrInt = m_builder.CreateAdd(thisPtr, wideStartOffset, "fields_ptr_int");
			ctx.fieldPointer = m_builder.CreateIntToPtr(fieldPtrInt, m_pointerType, "fields_ptr");
		} else {
			// fieldOffset will only be null if this is a system class
			assert(func->enclosingClass->info->IsCppSystemClass() && "found null fieldOffset in non-system class");
		}
	}
	if (takesUpvaluePack) {
		ctx.upvaluePack = fnData->upvaluePackDef.get();
		ctx.upvaluePackPtr = function->getArg(nextArg++);
		ctx.upvaluePackPtr->setName("upvalue_pack");
	}

	// We need to copy the arguments into the local variables we made for them. However, if the local
	// variable is used by an upvalue, then this would fail as we'd be writing to uninitialised memory
	// since we haven't created the root-level upvalue yet.
	// Thus visit that node early - it'll no-op when we run it a second time.
	if (func->rootBeginUpvalues) {
		VisitStmtBeginUpvalues(&ctx, func->rootBeginUpvalues);
		func->rootBeginUpvalues->GetBackendData<BeginUpvaluesData>()->createdEarly = true;
	}

	for (LocalVariable *arg : func->parameters) {
		// Load the arguments
		llvm::Value *destPtr = GetLocalPointer(&ctx, arg);
		llvm::Value *value = function->getArg(nextArg++);
		value->setName(arg->Name());
		m_builder.CreateStore(value, destPtr);
	}

	VisitStmt(&ctx, func->body);

	m_dbgSubProgramme = nullptr;

	return function;
}

void LLVMBackendImpl::GenerateInitialiser(Module *mod) {
	llvm::BasicBlock *bb = llvm::BasicBlock::Create(m_context, "entry", m_initFunc);
	m_builder.SetInsertPoint(bb);

	// Make an attribute list that represents a pure function, which the optimiser can move around, duplicate or
	// remove calls to.
	llvm::MemoryEffects inaccessibleRead(llvm::MemoryEffects::InaccessibleMem, llvm::ModRefInfo::Ref);
	llvm::Attribute inaccReadAttr = llvm::Attribute::getWithMemoryEffects(m_context, inaccessibleRead);
	std::vector<std::pair<unsigned int, llvm::Attribute>> pureAttrs = {
	    {llvm::AttributeList::FunctionIndex, inaccReadAttr},
	    {llvm::AttributeList::FunctionIndex, llvm::Attribute::get(m_context, llvm::Attribute::NoUnwind)},
	    {llvm::AttributeList::FunctionIndex, llvm::Attribute::get(m_context, llvm::Attribute::WillReturn)},
	};
	llvm::AttributeList pureFunc = llvm::AttributeList::get(m_context, pureAttrs);

	// Remove any unused system variables, for ease of reading the LLVM IR
	// Make an exception for Obj, since it's used below for the class declaration stuff
	for (const auto &entry : ExprSystemVar::SYSTEM_VAR_NAMES) {
		llvm::GlobalVariable *var = m_systemVars.at(entry.first);
		if (m_usedSystemVars.contains(var) || entry.first == "Object")
			continue;
		m_systemVars.erase(entry.first);
		m_module.getGlobalList().erase(var);
	}

	// Load the variables for all the core values
	std::vector<llvm::Type *> argTypes = {m_pointerType};
	llvm::FunctionType *sysLookupType = llvm::FunctionType::get(m_valueType, argTypes, false);
	llvm::FunctionCallee getSysVarFn =
	    m_module.getOrInsertFunction("wren_get_core_class_value", sysLookupType, pureFunc);

	for (const auto &entry : m_systemVars) {
		std::vector<llvm::Value *> args = {GetStringConst(entry.first)};
		llvm::Value *result = m_builder.CreateCall(getSysVarFn, args, "var_" + entry.first);

		m_builder.CreateStore(result, entry.second);
	}

	// Load the boolean values
	llvm::FunctionType *getBoolType = llvm::FunctionType::get(m_valueType, {m_int8Type}, false);
	llvm::FunctionCallee getBoolFn = m_module.getOrInsertFunction("wren_get_bool_value", getBoolType, pureFunc);
	llvm::Value *trueValue = m_builder.CreateCall(getBoolFn, {CInt::get(m_int8Type, true)}, "true_value");
	m_builder.CreateStore(trueValue, m_valueTruePtr);
	llvm::Value *falseValue = m_builder.CreateCall(getBoolFn, {CInt::get(m_int8Type, false)}, "false_value");
	m_builder.CreateStore(falseValue, m_valueFalsePtr);

	// Create all the string constants

	argTypes = {m_pointerType, m_pointerType, m_int32Type};
	llvm::FunctionType *newStringType = llvm::FunctionType::get(m_valueType, argTypes, false);
	llvm::FunctionCallee newStringFn = m_module.getOrInsertFunction("wren_init_string_literal", newStringType);

	for (const auto &entry : m_managedStrings) {
		// Create a raw C string
		llvm::Constant *strPtr = GetStringConst(entry.first);

		// And construct a string object from it
		std::vector<llvm::Value *> args = {m_getGlobals, strPtr, CInt::get(m_int32Type, entry.first.size())};
		llvm::Value *value = m_builder.CreateCall(newStringFn, args);

		m_builder.CreateStore(value, entry.second);
	}

	// Register the upvalues, creating ClosureSpec-s for each closure
	argTypes = {m_pointerType};
	llvm::FunctionType *newClosureType = llvm::FunctionType::get(m_pointerType, argTypes, false);
	llvm::FunctionCallee newClosureFn = m_module.getOrInsertFunction("wren_register_closure", newClosureType);
	for (IRFn *fn : mod->GetFunctions()) {
		const FnData *fnData = fn->GetBackendData<FnData>();

		// Only produce ClosureSpecs for closures
		if (fnData->closureSpec == nullptr)
			continue;

		// For each upvalue, tell the runtime about it and save the description object it gives us. This object
		// is then used to closure objects wrapping this function.

		UpvaluePackDef *upvaluePack = fnData->upvaluePackDef.get();
		int numUpvalues = upvaluePack->variables.size();
		int numStorageLocations = upvaluePack->storageLocations.size();

		std::vector<llvm::Type *> specTypes = {
		    m_pointerType, m_pointerType, m_int32Type, m_int32Type, m_int32Type, m_int32Type};
		specTypes.insert(specTypes.end(), numUpvalues, m_int32Type); // Add the upvalue indices
		llvm::StructType *closureSpecType = llvm::StructType::get(m_context, specTypes);

		// Generate the spec table
		std::vector<llvm::Constant *> structContent = {
		    fnData->llvmFunc,                              // function pointer
		    GetStringConst(fn->debugName),                 // name C string
		    CInt::get(m_int32Type, fn->parameters.size()), // Arity
		    CInt::get(m_int32Type, numUpvalues),           // Upvalue count
		    CInt::get(m_int32Type, numStorageLocations),   // Storage pack pointers count
		    CInt::get(m_int32Type, 0),                     // Unused padding
		};
		for (UpvalueVariable *upvalue : upvaluePack->variables) {
			// Just generate zeros, since we fill the upvalue pack in generated code
			// TODO remove the requirement to write out these variable indexes
			structContent.push_back(CInt::get(m_int32Type, 0));
		}

		llvm::Constant *constant = llvm::ConstantStruct::get(closureSpecType, structContent);

		llvm::GlobalVariable *specData = new llvm::GlobalVariable(m_module, constant->getType(), true,
		    llvm::GlobalVariable::PrivateLinkage, constant, "closure_spec_" + fn->debugName);

		// And generate the registration code
		std::vector<llvm::Value *> args = {specData};
		llvm::Value *spec = m_builder.CreateCall(newClosureFn, args, fn->debugName);
		m_builder.CreateStore(spec, fnData->closureSpec);
	}

	for (IRClass *cls : mod->GetClasses()) {
		using CD = ClassDescription;
		using Cmd = ClassDescription::Command;

		ClassData *classData = cls->GetBackendData<ClassData>();

		// Build the data as a list of i64s
		std::vector<llvm::Constant *> values;

		auto addCmdFlag = [this, &values](Cmd cmd, int flags) {
			// This is the same (with little-endian) as two i32s: <ADD_METHOD> <flags>
			uint64_t cmdFlag = (uint64_t)cmd + ((uint64_t)flags << 32);
			values.push_back(CInt::get(m_int64Type, cmdFlag));
		};

		// If this class is supposed to add methods to one of the C++ classes, set that up
		if (cls->info->IsCppSystemClass()) {
			addCmdFlag(Cmd::MARK_SYSTEM_CLASS, 0);
		}

		if (cls->info->isForeign) {
			addCmdFlag(Cmd::MARK_FOREIGN_CLASS, 0);
		}

		// Write the attributes - both for the class, and all the methods
		auto writeSingleAttribute = [&](const std::string &name, const AttrContent &attr) {
			llvm::Constant *nameConst = GetStringConst(name);
			values.push_back(llvm::ConstantExpr::getPtrToInt(nameConst, m_int64Type));

			CD::AttrType type;
			llvm::Constant *result;

			if (attr.str) {
				type = CD::AttrType::STRING;
				result = llvm::ConstantExpr::getPtrToInt(GetStringConst(attr.str.value()), m_int64Type);
			} else if (attr.boolean) {
				type = CD::AttrType::BOOLEAN;
				result = CInt::get(m_int64Type, attr.boolean.value());
			} else {
				type = CD::AttrType::VALUE;
				result = CInt::get(m_int64Type, attr.value);
			}

			values.push_back(CInt::get(m_int64Type, (uint64_t)type));

			values.push_back(result);
		};
		auto writeAttributes = [&](const AttributePack *attributes, int method) {
			if (!attributes)
				return;

			// We emit attribute blocks per-group, so group the keys by them
			std::map<std::string, std::vector<const AttrKey *>> byGroup;
			for (const auto &[key, _] : attributes->attributes) {
				byGroup[key.group].push_back(&key);
			}

			for (const auto &[group, keys] : byGroup) {
				// Find exactly how many attributes we'll be writing
				std::vector<std::pair<std::string, const AttrContent *>> contents;
				for (const AttrKey *key : keys) {
					for (const AttrContent &content : attributes->attributes.at(*key)) {
						if (!content.runtimeAccess)
							continue;
						contents.push_back({key->name, &content});
					}
				}

				// If we don't have any to write, then skip this group
				if (contents.empty())
					continue;

				addCmdFlag(Cmd::ADD_ATTRIBUTE_GROUP, CD::FLAG_NONE);

				llvm::Constant *groupConst = GetStringConst(group);
				values.push_back(llvm::ConstantExpr::getPtrToInt(groupConst, m_int64Type));

				// On little-endian layouts, this is <i32 method> <i32 size>
				// Note we cast the method to a uint32_t first, so it doesn't get sign-extended
				uint64_t methodAndCount = ((uint32_t)method) + (contents.size() << 32);
				values.push_back(CInt::get(m_int64Type, methodAndCount)); // Count

				for (const auto &[name, content] : contents) {
					writeSingleAttribute(name, *content);
				}
			}
		};

		// Write the class attributes here, the rest will be mixed in with the methods
		writeAttributes(cls->info->attributes.get(), -1);
		int methodId = 0; // Used to track the method index for the attribute data

		// Emit a block for each method
		auto writeMethods = [&](const ClassInfo::MethodMap &methods, bool isStatic) {
			for (const auto &[sig, method] : methods) {
				int flags = 0;
				if (isStatic)
					flags |= CD::FLAG_STATIC;
				if (method->isForeign)
					flags |= CD::FLAG_FOREIGN;
				addCmdFlag(Cmd::ADD_METHOD, flags);

				llvm::Constant *strConst = GetStringConst(sig->ToString());
				values.push_back(llvm::ConstantExpr::getPtrToInt(strConst, m_int64Type));

				// Native methods use a 'stub' function which calls out to the runtime.
				llvm::Constant *funcPtr;
				if (method->fn) {
					FnData *fnData = method->fn->GetBackendData<FnData>();
					funcPtr = fnData->llvmFunc;
				} else {
					const auto &stubs = method->isStatic ? classData->foreignStaticStubs : classData->foreignStubs;
					auto iter = stubs.find(method->signature);
					assert(iter != stubs.end() && "Foreign stub not generated for foreign method");
					funcPtr = iter->second;
				}
				values.push_back(llvm::ConstantExpr::getPtrToInt(funcPtr, m_int64Type));

				writeAttributes(method->attributes.get(), methodId);
				methodId++;
			}
		};

		writeMethods(cls->info->methods, false);
		writeMethods(cls->info->staticMethods, true);

		for (const std::unique_ptr<FieldVariable> &var : cls->info->fields.fields) {
			addCmdFlag(Cmd::ADD_FIELD, CD::FLAG_NONE);
			llvm::Constant *strConst = GetStringConst(var->Name());
			values.push_back(llvm::ConstantExpr::getPtrToInt(strConst, m_int64Type));
		}

		addCmdFlag(Cmd::END, CD::FLAG_NONE); // Signal the end of the class description

		llvm::ArrayType *dataBlockType = llvm::ArrayType::get(m_int64Type, values.size());
		llvm::Constant *dataConstant = llvm::ConstantArray::get(dataBlockType, values);
		llvm::GlobalVariable *classDataBlock = new llvm::GlobalVariable(m_module, dataBlockType, true,
		    llvm::GlobalVariable::PrivateLinkage, dataConstant, "class_data_" + cls->info->name);

		// See the Generate function where the global is created for an explanation of why we're
		// pointing a global variable at another global variable.
		classData->classDataBlock->setInitializer(classDataBlock);
	}

	// Register the signatures table
	// This is just a concatenated list of null-terminated strings, with one last null at the end.
	{
		std::vector<uint8_t> sigTable;

		for (const std::string &str : m_signatures) {
			sigTable.insert(sigTable.end(), str.begin(), str.end());
			sigTable.push_back(0);
		}
		sigTable.push_back(0);

		llvm::Constant *constant = llvm::ConstantDataArray::get(m_context, sigTable);
		llvm::GlobalVariable *var = new llvm::GlobalVariable(m_module, constant->getType(), true,
		    llvm::GlobalVariable::PrivateLinkage, constant, "signatures_table");
		m_builder.CreateCall(m_registerSignatureTable, {var});
	}

	// Functions must return!
	m_builder.CreateRetVoid();

	// Finally, generate the get-global-table function. Do this even if the module doesn't have a name, as this
	// is how we tell the standalone executable stub about our main function.
	{
		m_builder.SetInsertPoint(llvm::BasicBlock::Create(m_context, "entry", m_getGlobals));

		// Create the globals table
		std::vector<llvm::Constant *> components;
		for (IRGlobalDecl *global : mod->GetGlobalVariables()) {
			components.push_back(GetStringConst(global->Name()));
			components.push_back(GetGlobalVariable(global));
		}

		// Add some special values that have a meaning to the runtime
		components.push_back(GetStringConst("<INTERNAL>::init_func"));
		components.push_back(mod->GetMainFunction()->GetBackendData<FnData>()->llvmFunc);
		components.push_back(GetStringConst("<INTERNAL>::module_name"));
		components.push_back(GetStringConst(mod->Name()));
		if (m_enableStatepoints) {
			// WrenGCMetadataPrinter writes out a symbol of this name at the start of its stack map region
			// Specify external linkage, but that gets ignored since we set it as dso_local anyway
			llvm::GlobalVariable *global = new llvm::GlobalVariable(m_module, m_pointerType, true,
			    llvm::GlobalVariable::ExternalLinkage, nullptr, WrenGCMetadataPrinter::GetStackMapSymbol());
			global->setDSOLocal(true);

			components.push_back(GetStringConst("<INTERNAL>::stack_map"));
			components.push_back(global);
		}

		// Terminating null
		components.push_back(m_nullPointer);

		llvm::ArrayType *arrayType = llvm::ArrayType::get(m_pointerType, components.size());
		llvm::Constant *value = llvm::ConstantArray::get(arrayType, components);
		llvm::GlobalVariable *globalsTable = new llvm::GlobalVariable(m_module, arrayType, true,
		    llvm::GlobalVariable::PrivateLinkage, value, "globals_table");

		m_builder.CreateRet(globalsTable);
	}
}

void LLVMBackendImpl::GenerateForeignStubs(Module *mod) {
	// For each foreign method, generate a function we can jump to and pass
	// arguments to like usual, but that calls into the runtime passing the
	// method information and arguments which can then be dispatched to the
	// real native method.

	for (IRClass *cls : mod->GetClasses()) {
		ClassData *classData = cls->GetBackendData<ClassData>();

		auto process = [&](std::unordered_map<Signature *, llvm::Function *> &stubs, MethodInfo *method) {
			if (!method->isForeign)
				return;

			std::string funcName = cls->info->name + "::" + method->signature->ToString() + "_foreignStub";

			int arity = method->signature->arity;
			int cArity = arity + 1; // Always has a receiver
			if (!method->isStatic) {
				// The receiver ('this') value.
				arity++;
			}

			// The 'regular' arguments, that the user would see
			std::vector<llvm::Type *> funcArgs;
			funcArgs.insert(funcArgs.end(), arity, m_valueType);

			llvm::FunctionType *ft = llvm::FunctionType::get(m_valueType, funcArgs, false);
			llvm::Function *function = llvm::Function::Create(ft, llvm::Function::PrivateLinkage, funcName, &m_module);

			stubs[method->signature] = function;

			llvm::BasicBlock *bb = llvm::BasicBlock::Create(m_context, "entry", function);
			m_builder.SetInsertPoint(bb);

			// Create an on-stack array for the variables and stuff them in
			llvm::Value *array = m_builder.CreateAlloca(m_valueType, CInt::get(m_int32Type, cArity), "args_array");

			// Store the receiver if it's not in the arguments
			llvm::Value *clsObject = m_builder.CreateLoad(m_valueType, classData->object, "class_obj");
			if (method->isStatic) {
				m_builder.CreateStore(clsObject, array);
			}

			// Store each of the arguments
			for (int i = 0; i < arity; i++) {
				// If this is a static method we add the receiver ourselves, so the 1st
				// argument goes in the 2nd array position to leave room for it.
				int arrDest = method->isStatic ? i + 1 : i;

				llvm::Value *entryPtr = m_builder.CreateGEP(m_valueType, array, {CInt::get(m_int32Type, arrDest)});
				m_builder.CreateStore(function->getArg(i), entryPtr);
			}

			// Let the runtime call the native implementation. We make a global variable
			// in which the runtime will store the pointer to the foreign function, to
			// improve performance.
			llvm::GlobalVariable *cacheVar = new llvm::GlobalVariable(m_module, m_pointerType, false,
			    llvm::GlobalVariable::PrivateLinkage, m_nullPointer, "ff_cache_" + funcName);

			// Passing in our function pointer to allow the runtime to figure out who we are.
			llvm::Value *result = m_builder.CreateCall(m_callForeignMethod,
			    {cacheVar, array, CInt::get(m_int32Type, arity), clsObject, function}, "result");

			m_builder.CreateRet(result);
		};

		for (const auto &[_, method] : cls->info->methods) {
			process(classData->foreignStubs, method.get());
		}
		for (const auto &[_, method] : cls->info->staticMethods) {
			process(classData->foreignStaticStubs, method.get());
		}
	}
}

void LLVMBackendImpl::SetupDebugInfo(Module *mod) {
	// Clear everything out, in case it was left over from a previous module (for now we only use a backend instance
	// to compile a single module, but that might change at some point).
	m_debugInfo.reset();
	m_dbgFile = nullptr;
	m_dbgSubProgramme = nullptr;
	m_dbgCompileUnit = nullptr;
	m_dbgCurrentNode = nullptr;
	m_dbgValueType = nullptr;
	m_dbgUpvaluePackType = nullptr;

	if (!m_dbgEnable)
		return;

	m_debugInfo = std::make_unique<llvm::DIBuilder>(m_module);

	// Specify the version of the debug information we're generating - this is required if we're passing
	// our IR back into opt, and might be needed in other places too.
	m_module.addModuleFlag(llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);

	if (mod->sourceFilePath) {
		std::vector<std::string> filenameParts = utils::stringSplit(mod->sourceFilePath.value(), "/");
		std::string filenameNoDir = filenameParts.back();
		filenameParts.pop_back();
		std::string directory = utils::stringJoin(filenameParts, "/");
		m_dbgFile = m_debugInfo->createFile(filenameNoDir, directory);
	} else {
		m_dbgFile = m_debugInfo->createFile("<unknown-source>.wren", ".");
	}

	std::string producer = "qwrencc native compiler <todo options> <todo version>";
	// TODO isOptimised flag
	m_dbgCompileUnit = m_debugInfo->createCompileUnit(dwarf::DW_LANG_C, m_dbgFile, producer, false, "", 1);

	// It's a big ugly, we can't really create a proper type for values
	m_dbgValueType = m_debugInfo->createBasicType("Value", 64, dwarf::DW_ATE_float);

	m_dbgUpvaluePackType = m_debugInfo->createUnspecifiedType("UpvaluePackPtr");
}

llvm::Constant *LLVMBackendImpl::GetStringConst(const std::string &str) {
	auto iter = m_stringConstants.find(str);
	if (iter != m_stringConstants.end())
		return iter->second;

	std::vector<int8_t> data(str.size() + 1);
	std::copy(str.begin(), str.end(), data.begin());

	std::string name = "str_" + FilterStringLiteral(str);
	llvm::Constant *constant = llvm::ConstantDataArray::get(m_context, data);
	llvm::GlobalVariable *var = new llvm::GlobalVariable(m_module, constant->getType(), true,
	    llvm::GlobalVariable::PrivateLinkage, constant, name);

	m_stringConstants[str] = var;
	return var;
}

llvm::Value *LLVMBackendImpl::GetManagedStringValue(const std::string &str) {
	auto iter = m_managedStrings.find(str);
	if (iter != m_managedStrings.end())
		return iter->second;

	std::string name = "strobj_" + FilterStringLiteral(str);
	llvm::GlobalVariable *var =
	    new llvm::GlobalVariable(m_module, m_valueType, false, llvm::GlobalVariable::PrivateLinkage, m_nullValue, name);
	m_managedStrings[str] = var;
	return var;
}

llvm::GlobalVariable *LLVMBackendImpl::GetGlobalVariable(IRGlobalDecl *global) {
	auto iter = m_globalVariables.find(global);
	if (iter != m_globalVariables.end())
		return iter->second;

	llvm::GlobalVariable *var = new llvm::GlobalVariable(m_module, m_valueType, false,
	    llvm::GlobalVariable::PrivateLinkage, m_nullValue, "gbl_" + global->Name());
	m_globalVariables[global] = var;
	return var;
}

llvm::Value *LLVMBackendImpl::GetLocalPointer(VisitorContext *ctx, LocalVariable *local) {
	VarData *varData = local->GetBackendVarData<VarData>();

	if (varData->address)
		return varData->address;

	// Check if it's a closed-over variable
	if (varData->closedAddressPosition != -1) {
		// Find the StmtBeginUpvalues that this variable belongs to, as it
		// contains the pointer to the upvalue storage array.
		BeginUpvaluesData *upvalueData = local->beginUpvalues->GetBackendData<BeginUpvaluesData>();

		// Find the block position - this is reading the value from an alloca-d variable
		llvm::Value *storagePtr = m_builder.CreateLoad(m_pointerType, upvalueData->packPtr, "storage_ptr");

		std::vector<llvm::Value *> indices = {
		    // Select the item we're interested in.
		    CInt::get(m_int32Type, varData->closedAddressPosition + UPVALUE_STORAGE_OFFSET),
		};
		return m_builder.CreateGEP(m_valueType, storagePtr, indices, "lv_ptr_" + local->Name());
	}

	fmt::print(stderr, "Found unallocated local variable: '{}'\n", local->Name());
	abort();
}

llvm::Value *LLVMBackendImpl::GetUpvaluePointer(VisitorContext *ctx, UpvalueVariable *upvalue) {
	if (!ctx->upvaluePack) {
		fmt::print(stderr, "Found UpvalueVariable without an upvalue pack!\n");
		abort();
	}

	auto upvalueIter = ctx->upvaluePack->variableIds.find(upvalue);
	if (upvalueIter == ctx->upvaluePack->variableIds.end()) {
		fmt::print(stderr, "Could not find upvalue in current pack for variable {}\n", upvalue->parent->Name());
		abort();
	}
	int position = upvalueIter->second;

	// Get a pointer pointing to the position in the upvalue pack where this variable is.
	// Recall the upvalue pack is an array of pointers, each one pointing to a value.
	std::vector<llvm::Value *> args = {CInt::get(m_int32Type, position)};
	llvm::Value *varPtrPtr =
	    m_builder.CreateGEP(m_pointerType, ctx->upvaluePackPtr, args, "uv_pptr_" + upvalue->Name());

	// The upvalue pack stores pointers, so at this point our variable is a Value** and we have to dereference
	// it twice. In the future we should store never-modified variables directly, so this would be Value*.
	// This chases the pointer in-place, that is in the same variable, to avoid the bother of declaring another one.
	return m_builder.CreateLoad(m_pointerType, varPtrPtr, "uv_ptr_" + upvalue->Name());
}

llvm::Value *LLVMBackendImpl::GetVariablePointer(VisitorContext *ctx, VarDecl *var) {
	LocalVariable *local = dynamic_cast<LocalVariable *>(var);
	UpvalueVariable *upvalue = dynamic_cast<UpvalueVariable *>(var);
	IRGlobalDecl *global = dynamic_cast<IRGlobalDecl *>(var);

	if (local) {
		return GetLocalPointer(ctx, local);
	}
	if (upvalue) {
		return GetUpvaluePointer(ctx, upvalue);
	}
	if (global) {
		return GetGlobalVariable(global);
	}
	fprintf(stderr, "Attempted to access non-local, non-global, non-upvalue variable '%s'\n", var->Name().c_str());
	abort();
}

llvm::BasicBlock *LLVMBackendImpl::GetLabelBlock(VisitorContext *ctx, StmtLabel *label) {
	if (ctx->labelBlocks.contains(label))
		return ctx->labelBlocks.at(label);

	// Don't supply the function - we'll add it when we find the StmtLabel, so it'll be in the proper order
	// when read manually. The order of the BBs shouldn't matter (AFAIK) to the compiler.
	llvm::BasicBlock *block = llvm::BasicBlock::Create(m_context, "lbl_" + label->debugName);

	ctx->labelBlocks[label] = block;
	return block;
}

llvm::Value *LLVMBackendImpl::GetObjectFieldPointer(VisitorContext *ctx, VarDecl *thisVar) {
	// The pointer to the start of our class's field array.
	// This is either set at the start of the method, or loaded
	// here in the case of closures.
	if (!thisVar) {
		return ctx->fieldPointer;
	}

	// Get the 'this' value and convert it to a pointer.
	// There's no error checking here for performance, we'll probably
	// get a fault if something is wrong (though we can't count on
	// it from a security perspective).
	llvm::Value *thisValuePtr = GetVariablePointer(ctx, thisVar);
	llvm::Value *thisValue = m_builder.CreateLoad(m_valueType, thisValuePtr, "this_value");
	llvm::Value *thisPtr =
	    m_builder.CreateCall(m_ptrMask, {thisValue, CInt::get(m_int64Type, CONTENT_MASK)}, "this_ptr");

	// Find the class containing this closure
	IRClass *cls = nullptr;
	for (IRFn *fn = ctx->currentWrenFunc; fn; fn = fn->parent) {
		cls = fn->enclosingClass;
		if (cls)
			break;
	}
	assert(cls && "Found closure without an enclosing class");

	// Use CreateGEP with i8 to add a number of bytes, that being the field offset.
	ClassData *cd = cls->GetBackendData<ClassData>();
	llvm::Value *offsetNumber = m_builder.CreateLoad(m_int32Type, cd->fieldOffset, "this_field_offset");
	return m_builder.CreateGEP(m_int8Type, thisPtr, {offsetNumber});
}

std::string LLVMBackendImpl::FilterStringLiteral(const std::string &literal) {
	// Limit the maximum string length
	int length = std::min((int)literal.size(), 30);
	std::string result = literal.substr(0, length);

	// Go through and remove any null bytes - they're a valid string character, but LLVM
	// is understandably unimpressed by them.
	size_t lastNull = 0;
	while (true) {
		lastNull = result.find((char)0, lastNull);
		if (lastNull == std::string::npos)
			break;
		result.erase(lastNull, 1);
	}

	return result;
}

ScopedDebugGuard::ScopedDebugGuard(LLVMBackendImpl *backend, IRNode *node) : m_backend(backend) {
	m_previous = backend->m_dbgCurrentNode;
	m_backend->WriteNodeDebugInfo(node);
}
ScopedDebugGuard::~ScopedDebugGuard() { m_backend->WriteNodeDebugInfo(m_previous); }

// Visitors
ExprRes LLVMBackendImpl::VisitExpr(VisitorContext *ctx, IRExpr *expr) {
	ScopedDebugGuard debugBlock(this, expr);

#define DISPATCH(func, type)                                                                                           \
	do {                                                                                                               \
		if (typeid(*expr) == typeid(type))                                                                             \
			return func(ctx, dynamic_cast<type *>(expr));                                                              \
	} while (0)

	// Use both the function name and type for ease of searching and IDE indexing
	DISPATCH(VisitExprConst, ExprConst);
	DISPATCH(VisitExprLoad, ExprLoad);
	DISPATCH(VisitExprFieldLoad, ExprFieldLoad);
	DISPATCH(VisitExprFuncCall, ExprFuncCall);
	DISPATCH(VisitExprClosure, ExprClosure);
	DISPATCH(VisitExprLoadReceiver, ExprLoadReceiver);
	DISPATCH(VisitExprRunStatements, ExprRunStatements);
	DISPATCH(VisitExprAllocateInstanceMemory, ExprAllocateInstanceMemory);
	DISPATCH(VisitExprSystemVar, ExprSystemVar);
	DISPATCH(VisitExprGetClassVar, ExprGetClassVar);

#undef DISPATCH

	fmt::print("Unknown expr node {}\n", typeid(*expr).name());
	abort();
	return {};
}

StmtRes LLVMBackendImpl::VisitStmt(VisitorContext *ctx, IRStmt *expr) {
	ScopedDebugGuard debugBlock(this, expr);

#define DISPATCH(func, type)                                                                                           \
	do {                                                                                                               \
		if (typeid(*expr) == typeid(type))                                                                             \
			return func(ctx, dynamic_cast<type *>(expr));                                                              \
	} while (0)

	DISPATCH(VisitStmtAssign, StmtAssign);
	DISPATCH(VisitStmtFieldAssign, StmtFieldAssign);
	DISPATCH(VisitStmtEvalAndIgnore, StmtEvalAndIgnore);
	DISPATCH(VisitBlock, StmtBlock);
	DISPATCH(VisitStmtLabel, StmtLabel);
	DISPATCH(VisitStmtJump, StmtJump);
	DISPATCH(VisitStmtReturn, StmtReturn);
	DISPATCH(VisitStmtLoadModule, StmtLoadModule);
	DISPATCH(VisitStmtBeginUpvalues, StmtBeginUpvalues);
	DISPATCH(VisitStmtRelocateUpvalues, StmtRelocateUpvalues);
	DISPATCH(VisitStmtDefineClass, StmtDefineClass);

#undef DISPATCH

	fmt::print("Unknown stmt node {}\n", typeid(*expr).name());
	abort();
	return {};
}

void LLVMBackendImpl::WriteNodeDebugInfo(IRNode *node) {
	if (!m_dbgEnable)
		return;

	if (!node) {
		// Used in the function prologue, to tell the debugger that it's not user-defined code
		m_builder.SetCurrentDebugLocation(llvm::DebugLoc());
		return;
	}

	IRDebugInfo &di = node->debugInfo;

	// If this node doesn't bear any direct resemblance to the source code, don't write debug information for it
	if (di.synthetic)
		return;

	if (di.lineNumber == -1) {
		fprintf(stderr, "dbginfo: Missing line num for node %s\n", typeid(*node).name());
		abort();
	}

	m_builder.SetCurrentDebugLocation(
	    llvm::DILocation::get(m_dbgCompileUnit->getContext(), di.lineNumber, di.column, m_dbgSubProgramme));
}

ExprRes LLVMBackendImpl::VisitExprConst(VisitorContext *ctx, ExprConst *node) {
	llvm::Value *value;
	switch (node->value.type) {
	case CcValue::NULL_TYPE: {
		// We can't use m_nullValue in a function body, we have to use the dummy cast function so as
		// to avoid confusing the RS4GC pass.
		value = m_builder.CreateCall(m_dummyPtrBitcast, {CInt::get(m_int64Type, NULL_VAL)});
		break;
	}
	case CcValue::STRING: {
		llvm::Value *ptr = GetManagedStringValue(node->value.s);
		std::string name = "strobj_" + FilterStringLiteral(node->value.s);
		value = m_builder.CreateLoad(m_valueType, ptr, name);
		break;
	}
	case CcValue::BOOL: {
		llvm::Value *ptr = node->value.b ? m_valueTruePtr : m_valueFalsePtr;
		std::string name = node->value.b ? "const_true" : "const_false";
		value = m_builder.CreateLoad(m_valueType, ptr, name);
		break;
	}
	case CcValue::INT:
	case CcValue::NUM: {
		// Since valueType is a pointer, we have to produce the value as an integer then cast it in
		llvm::Constant *intValue = CInt::get(m_int64Type, encode_number(node->value.n));
		value = m_builder.CreateCall(m_dummyPtrBitcast, {intValue});
		break;
	}
	default:
		fprintf(stderr, "Invalid constant node type %d\n", (int)node->value.type);
		abort();
		break;
	}

	return {value};
}
ExprRes LLVMBackendImpl::VisitExprLoad(VisitorContext *ctx, ExprLoad *node) {
	llvm::Value *ptr = GetVariablePointer(ctx, node->var);
	llvm::Value *value = m_builder.CreateLoad(m_valueType, ptr, node->var->Name() + "_value");
	return {value};
}
ExprRes LLVMBackendImpl::VisitExprFieldLoad(VisitorContext *ctx, ExprFieldLoad *node) {
	llvm::Value *objFieldsPointer = GetObjectFieldPointer(ctx, node->thisVar);
	llvm::Value *fieldPointer = m_builder.CreateGEP(m_valueType, objFieldsPointer,
	    {CInt::get(m_int32Type, node->var->Id())}, "field_ptr_" + node->var->Name());

	llvm::Value *result = m_builder.CreateLoad(m_valueType, fieldPointer, "field_" + node->var->Name());
	return {result};
}
ExprRes LLVMBackendImpl::VisitExprFuncCall(VisitorContext *ctx, ExprFuncCall *node) {
	ExprRes receiver = VisitExpr(ctx, node->receiver);

	std::vector<llvm::Value *> args;
	args.push_back(receiver.value);
	for (IRExpr *expr : node->args) {
		ExprRes res = VisitExpr(ctx, expr);
		args.push_back(res.value);
	}

	std::string name = node->signature->ToString();
	m_signatures.insert(name);
	SignatureId signature = hash_util::findSignatureId(name);
	llvm::Value *sigValue = CInt::get(m_signatureType, signature.id);

	// Call the lookup function
	llvm::CallInst *func;
	if (!node->superCaller) {
		std::vector<llvm::Value *> lookupArgs = {receiver.value, sigValue};
		func = m_builder.CreateCall(m_virtualMethodLookup, lookupArgs, "vptr_" + name);
	} else {
		// Super lookups are special - we pass in the class this method is declared on, so the lookup function
		// can find the correct method.
		IRClass *cls = node->superCaller->enclosingClass;

		// Note that the receiver might be an upvalue, since you can make super calls from closures.

		llvm::GlobalVariable *classVar = cls->GetBackendData<ClassData>()->object;
		llvm::Value *thisClass = m_builder.CreateLoad(m_valueType, classVar, "super_cls_" + cls->info->name);

		llvm::Value *isStatic = CInt::get(m_int8Type, node->superCaller->methodInfo->isStatic);

		std::vector<llvm::Value *> lookupArgs = {receiver.value, thisClass, sigValue, isStatic};
		func = m_builder.CreateCall(m_superMethodLookup, lookupArgs, "vptr_" + name);
	}

	// Make the function type - TODO cache
	std::vector<llvm::Type *> argTypes(args.size(), m_valueType);
	llvm::FunctionType *type = llvm::FunctionType::get(m_valueType, argTypes, false);

	// Invoke it
	llvm::CallInst *result = m_builder.CreateCall(type, func, args, "result_" + name);

	return {result};
}
ExprRes LLVMBackendImpl::VisitExprClosure(VisitorContext *ctx, ExprClosure *node) {
	// Create the closure, passing in null for the two upvalue-related arguments, since we'll populate them here
	llvm::Value *closureSpec = node->func->GetBackendData<FnData>()->closureSpec;
	llvm::Value *specObj = m_builder.CreateLoad(m_pointerType, closureSpec, "closure_spec_" + node->func->debugName);
	std::vector<llvm::Value *> args = {specObj, m_nullPointer, m_nullPointer, m_nullPointer};
	llvm::Value *closure = m_builder.CreateCall(m_createClosure, args, "closure_" + node->func->debugName);

	FnData *fnData = node->func->GetBackendData<FnData>();
	UpvaluePackDef *upvaluePack = fnData->upvaluePackDef.get();

	// If this function takes any upvalues, then fill them appropriately
	if (upvaluePack->variables.empty())
		return {closure};

	llvm::Value *upvaluePackPtr = m_builder.CreateCall(m_getUpvaluePack, {closure}, "uv_pack_" + node->func->debugName);

	IRFn *currentFunc = ctx->currentWrenFunc;

	// Load all the upvalues
	for (int uvSlot = 0; uvSlot < upvaluePack->variables.size(); uvSlot++) {
		UpvalueVariable *upvalue = upvaluePack->variables.at(uvSlot);
		assert(currentFunc == node->func->parent);

		// Get the pointer to the location where we have to store the upvalue pack to
		llvm::Value *targetSlotPtr = m_builder.CreateGEP(m_pointerType, upvaluePackPtr,
		    {CInt::get(m_int32Type, uvSlot)}, "uv_slot_" + upvalue->Name());

		LocalVariable *target = dynamic_cast<LocalVariable *>(upvalue->parent);
		if (target != nullptr) {
			llvm::Value *localPtr = GetLocalPointer(ctx, target);
			m_builder.CreateStore(localPtr, targetSlotPtr); // NOLINT(readability-suspicious-call-argument)
			continue;
		}

		// This upvalue must point to another upvalue, right?
		UpvalueVariable *nested = dynamic_cast<UpvalueVariable *>(upvalue->parent);
		if (!nested) {
			fmt::print(stderr, "Upvalue {} has non-local, non-upvalue parent scope {}.\n", upvalue->Name(),
			    upvalue->parent->Scope());
			abort();
		}

		// This upvalue should belong to this function
		assert(nested->containingFunction == currentFunc);

		llvm::Value *valuePtr = GetUpvaluePointer(ctx, nested);
		m_builder.CreateStore(valuePtr, targetSlotPtr); // NOLINT(readability-suspicious-call-argument)
	}

	// Load the pointers to all the upvalue storage locations, - this is used for reference counting.
	for (const auto &[storage, location] : upvaluePack->storageLocations) {
		BeginUpvaluesData *beginUpvalues = storage->GetBackendData<BeginUpvaluesData>();

		llvm::Value *storageAddress;

		if (beginUpvalues->function == ctx->currentWrenFunc) {
			// This upvalue references something from the current
			// function - fetch the storage pointer directly.
			storageAddress = m_builder.CreateLoad(m_pointerType, beginUpvalues->packPtr, "storage_ptr");
		} else {
			// This storage block was declared in another function. This implies
			// it's an upvalue to an upvalue, so we'll have the current value
			// and storage pointers (though we only need the latter here) in our
			// upvalue pack.
			// This is similar to (and derived from) the GetUpvaluePointer function.

			UpvaluePackDef *thisFnUpvaluePack = ctx->currentWrenFunc->GetBackendData<FnData>()->upvaluePackDef.get();
			auto iter = thisFnUpvaluePack->storageLocations.find(storage);

			if (iter == thisFnUpvaluePack->storageLocations.end()) {
				fmt::print(stderr, "Upvalue in {} used StmtBeginUpvalues block not available from parent\n",
				    node->func->debugName);
				abort();
			}
			int position = iter->second;

			// Get a pointer pointing to the position in the upvalue pack where the pointer
			// to this storage block is. Recall the upvalue pack is an array of pointers.
			llvm::Value *storagePtrPtr = m_builder.CreateGEP(m_pointerType, ctx->upvaluePackPtr,
			    {CInt::get(m_int32Type, position)}, "uv_storage_" + beginUpvalues->function->debugName);

			// The upvalue pack stores pointers, so at this point our variable
			// is a StorageBlock** and we have to dereference it twice.
			storageAddress = m_builder.CreateLoad(m_pointerType, storagePtrPtr,
			    "uv_storage_ptr_" + beginUpvalues->function->debugName);
		}

		// Increment the storage block's reference count - it's stored in the first 32 bits.
		llvm::Value *originalRefCount = m_builder.CreateLoad(m_int32Type, storageAddress, "old_storage_refs");
		llvm::Value *newRefCount = m_builder.CreateAdd(originalRefCount, CInt::get(m_int32Type, 1), "new_refcount");
		m_builder.CreateStore(newRefCount, storageAddress);

		// Get the pointer to the location where we have to store the storage pointer to.
		llvm::ConstantInt *locationInt = CInt::get(m_int32Type, location);
		llvm::Value *targetSlotPtr = m_builder.CreateGEP(m_pointerType, upvaluePackPtr, {locationInt},
		    "storage_slot_" + std::to_string(location));

		m_builder.CreateStore(storageAddress, targetSlotPtr);
	}

	return {closure};
}
ExprRes LLVMBackendImpl::VisitExprLoadReceiver(VisitorContext *ctx, ExprLoadReceiver *node) {
	if (!ctx->receiver) {
		fmt::print(stderr, "Found VisitExprLoadReceiver in function {} without receiver!\n",
		    ctx->currentFunc->getName());
		abort();
	}
	return {ctx->receiver};
}
ExprRes LLVMBackendImpl::VisitExprRunStatements(VisitorContext *ctx, ExprRunStatements *node) {
	VisitStmt(ctx, node->statement);

	llvm::Value *ptr = GetLocalPointer(ctx, node->temporary);
	llvm::Value *value = m_builder.CreateLoad(m_valueType, ptr, "temp_value");

	return {value};
}
ExprRes LLVMBackendImpl::VisitExprAllocateInstanceMemory(VisitorContext *ctx, ExprAllocateInstanceMemory *node) {
	std::string name = node->target->info->name;

	ClassData *cd = node->target->GetBackendData<ClassData>();

	llvm::Value *value = m_builder.CreateLoad(m_valueType, cd->object, "cls_" + name);
	llvm::Value *newObj = m_builder.CreateCall(m_allocObject, {value}, "new_obj_" + name);

	return {newObj};
}
ExprRes LLVMBackendImpl::VisitExprSystemVar(VisitorContext *ctx, ExprSystemVar *node) {
	llvm::GlobalVariable *global = m_systemVars.at(node->name);
	m_usedSystemVars.insert(global);
	llvm::Value *value = m_builder.CreateLoad(m_valueType, global, "gbl_" + node->name);
	return {value};
}
ExprRes LLVMBackendImpl::VisitExprGetClassVar(VisitorContext *ctx, ExprGetClassVar *node) {
	ClassData *cd = node->cls->GetBackendData<ClassData>();
	llvm::Value *value = m_builder.CreateLoad(m_valueType, cd->object, "cls_" + node->cls->info->name);
	return {value};
}

// Statements

StmtRes LLVMBackendImpl::VisitStmtAssign(VisitorContext *ctx, StmtAssign *node) {
	llvm::Value *value = VisitExpr(ctx, node->expr).value;

	llvm::Value *ptr = GetVariablePointer(ctx, node->var);
	m_builder.CreateStore(value, ptr);

	return {};
}
StmtRes LLVMBackendImpl::VisitStmtFieldAssign(VisitorContext *ctx, StmtFieldAssign *node) {
	ExprRes res = VisitExpr(ctx, node->value);

	llvm::Value *objFieldsPointer = GetObjectFieldPointer(ctx, node->thisVar);
	llvm::Value *fieldPointer = m_builder.CreateGEP(m_valueType, objFieldsPointer,
	    {CInt::get(m_int32Type, node->var->Id())}, "field_ptr_" + node->var->Name());

	m_builder.CreateStore(res.value, fieldPointer);
	return {};
}
StmtRes LLVMBackendImpl::VisitStmtEvalAndIgnore(VisitorContext *ctx, StmtEvalAndIgnore *node) {
	VisitExpr(ctx, node->expr);
	return {};
}
StmtRes LLVMBackendImpl::VisitBlock(VisitorContext *ctx, StmtBlock *node) {
	for (IRStmt *stmt : node->statements) {
		VisitStmt(ctx, stmt);
	}
	return {};
}
StmtRes LLVMBackendImpl::VisitStmtLabel(VisitorContext *ctx, StmtLabel *node) {
	llvm::BasicBlock *block = GetLabelBlock(ctx, node);

	// Create the fallthrough branch, but only if the last instruction wasn't a terminator
	bool isTerminator = false;
	if (!m_builder.GetInsertBlock()->empty()) {
		llvm::Instruction &last = m_builder.GetInsertBlock()->back();
		isTerminator = last.isTerminator();
	}
	if (!isTerminator) {
		m_builder.CreateBr(block);
	}

	// Add in the new block
	block->insertInto(ctx->currentFunc);
	m_builder.SetInsertPoint(block);

	return {};
}
StmtRes LLVMBackendImpl::VisitStmtJump(VisitorContext *ctx, StmtJump *node) {
	if (node->condition) {
		ExprRes res = VisitExpr(ctx, node->condition);
		llvm::Value *toCheck = res.value;

		// We have to create a fallthrough block, since our jump node falls through if the condition is false
		llvm::BasicBlock *fallthrough = llvm::BasicBlock::Create(m_context, "fallthrough", ctx->currentFunc);

		// Find the value of 'false'
		llvm::Value *falseValue = m_builder.CreateLoad(m_valueType, m_valueFalsePtr, "false_value");

		// This can be either null or false, which unfortunately is a bit of a pain
		llvm::Value *isNull = m_builder.CreateICmpEQ(toCheck, m_nullValue, "is_null");
		llvm::Value *isFalse = m_builder.CreateICmpEQ(toCheck, falseValue, "is_false");
		llvm::Value *isFalseyValue = m_builder.CreateOr(isNull, isFalse);

		llvm::BasicBlock *trueBB = GetLabelBlock(ctx, node->target);
		llvm::BasicBlock *falseBB = fallthrough;
		if (node->jumpOnFalse) {
			std::swap(trueBB, falseBB);
		}

		// Note the arguments for CreateCondBr is in the order trueBlock,falseBlock. However since we're checking if
		// our value is false, the 'true' condition will get called for a null/false value.
		m_builder.CreateCondBr(isFalseyValue, falseBB, trueBB);

		// Switch to our newly-made fallthrough block
		m_builder.SetInsertPoint(fallthrough);
	} else {
		// Unconditional branch, very easy :)
		m_builder.CreateBr(GetLabelBlock(ctx, node->target));
	}

	return {};
}
StmtRes LLVMBackendImpl::VisitStmtReturn(VisitorContext *ctx, StmtReturn *node) {
	ExprRes value = VisitExpr(ctx, node->value);
	m_builder.CreateRet(value.value);
	return {};
}
StmtRes LLVMBackendImpl::VisitStmtLoadModule(VisitorContext *ctx, StmtLoadModule *node) {
	// TODO move the name resolution stuff out of this backend, to somewhere backend-independent

	// Find the full name of this module. Stuff like './' will refer to a virtual directory structure in
	// which all the modules sit.
	std::string currentModName = ctx->currentModule->Name();
	std::string currentModDir;
	size_t lastStrokePos = currentModName.find_last_of('/');
	if (lastStrokePos != std::string::npos) {
		// Include the trailing stroke
		currentModDir = currentModName.substr(0, lastStrokePos + 1);
	}

	// At this point, currentModDir will be something like 'a/b/' if our module is named 'a/b/c'. If our module
	// is named 'a' then it's an empty string.

	std::string modName = node->importNode->moduleName;
	// If a module name starts with / then it's an absolute path
	if (modName.starts_with("/")) {
		modName = modName.substr(1);
	} else {
		// Note we'll pretend to be a bit more like a filesystem to satisfy a test case, so duplicate strokes and /./
		// are both allowed, and we just normalise them out here.
		modName = currentModDir + "/" + modName; // Get something like current_dir/./my_omd

		// It's easier to process the path if we turn it into a list of path segments
		std::vector<std::string> parts = utils::stringSplit(modName, "/");

		// Get rid of empty segments '//' and . segments '/./'
		// Iterate backwards since we're removing stuff
		for (ssize_t i = parts.size() - 1; i >= 0; i--) {
			const std::string &part = parts.at(i);
			if (part == "." || part.empty()) {
				parts.erase(parts.begin() + i);
			}
		}

		// If we see .. then remove the previous path segment. Do this in order, so multiple ..s in a row are handled
		// properly (removing two normal path segments, not each other).
		for (ssize_t i = 0; i < (ssize_t)parts.size(); i++) {
			if (parts.at(i) != "..")
				continue;

			// If we find a .. at the start, that's the user's fault.
			if (i == 0) {
				fmt::print(stderr, "Module path {} tries to reach before the module area root", modName);
				abort();
			}

			if (parts.at(i - 1) == "..") {
				fmt::print(stderr, "Error while processing module path {}: attempted to cancel ..", modName);
				abort();
			}

			// Remove both this and the previous item
			parts.erase(parts.begin() + i - 1, parts.begin() + i + 1);

			// Decrement i by two, since we've moved everything else back by two - otherwise we'd end up
			// skipping over two elements.
			i -= 2;
		}

		modName = utils::stringJoin(parts, "/");
	}

	// Create an import for that module's global-getter function
	llvm::FunctionType *funcType = llvm::FunctionType::get(m_pointerType, false);
	llvm::FunctionCallee globalsFunc = m_module.getOrInsertFunction(modName + "_get_globals", funcType);

	// Create or get the module - this runs it's initialiser, etc
	std::vector<llvm::Value *> args = {GetStringConst(modName), globalsFunc.getCallee()};
	llvm::Value *modPtr = m_builder.CreateCall(m_importModule, args, "module_" + node->importNode->moduleName);

	// And import all the variables from it
	for (const StmtLoadModule::VarImport &var : node->variables) {
		llvm::Value *varValue =
		    m_builder.CreateCall(m_getModuleVariable, {modPtr, GetStringConst(var.name)}, "mod_var_" + var.name);

		llvm::Value *destPtr = GetVariablePointer(ctx, var.bindTo);
		m_builder.CreateStore(varValue, destPtr);
	}

	return {};
}
StmtRes LLVMBackendImpl::VisitStmtBeginUpvalues(VisitorContext *ctx, StmtBeginUpvalues *node) {
	BeginUpvaluesData *upvaluesData = node->GetBackendData<BeginUpvaluesData>();

	assert(upvaluesData->contents.size() == node->variables.size());

	// We visit the first statement early so it's available when we load the function arguments, so
	// this might get run twice. In that case, do nothing the second time.
	if (upvaluesData->createdEarly)
		return {};

	// Allocate the storage block for the upvalued variables.
	CInt *count = CInt::get(m_int32Type, upvaluesData->contents.size());
	llvm::Value *allocatedBlock = m_builder.CreateCall(m_allocUpvalueStorage, {count}, "upvalueStorage");
	m_builder.CreateStore(allocatedBlock, upvaluesData->packPtr);

	return {};
}
StmtRes LLVMBackendImpl::VisitStmtRelocateUpvalues(VisitorContext *ctx, StmtRelocateUpvalues *node) {
	// All upvalues in the LLVM implementation are stored on the heap, since things get too complicated with
	// nested closures otherwise. All we have to do is decrement the reference count of the memory storing those values.

	for (StmtBeginUpvalues *beginning : node->upvalueSets) {
		BeginUpvaluesData *beginData = beginning->GetBackendData<BeginUpvaluesData>();

		llvm::Value *storagePtr = m_builder.CreateLoad(m_pointerType, beginData->packPtr, "unref_storage_ptr");
		m_builder.CreateCall(m_unrefUpvalueStorage, {storagePtr});
	}

	return {};
}
StmtRes LLVMBackendImpl::VisitStmtDefineClass(VisitorContext *ctx, StmtDefineClass *node) {
	IRClass *cls = node->targetClass;
	const ClassData &data = *cls->GetBackendData<ClassData>();

	llvm::Value *supertype;
	if (!cls->info->parentClass) {
		// Inherits from Object by default
		supertype = m_builder.CreateLoad(m_valueType, m_systemVars.at("Object"), "obj_class");
	} else {
		supertype = VisitExpr(ctx, cls->info->parentClass).value;
	}

	// The data block stored in the ClassData is a pointer to the actual data block. See where classDataBlock is
	// initialised for an explanation.
	llvm::Value *dataBlock = m_builder.CreateLoad(m_pointerType, data.classDataBlock, "class_data_" + cls->info->name);

	llvm::Constant *className = GetStringConst(cls->info->name);
	llvm::Value *classValue = m_builder.CreateCall(m_initClass, {m_getGlobals, className, dataBlock, supertype});

	// C++ system classes are registered, but we don't do anything with the result - we're just telling C++ what
	// methods exist on them.
	if (cls->info->IsCppSystemClass())
		return {};

	m_builder.CreateStore(classValue, data.object);

	// Look up the field offset - if this class is defined multiple times then we'll be repeating this, but one
	// would rather hope users won't declare inner classes inside hot loops!
	// In any case, the actual performance impact of this should be very small.
	llvm::Value *fieldOffsetValue =
	    m_builder.CreateCall(m_getClassFieldOffset, {classValue}, "field_offset_" + cls->info->name);
	m_builder.CreateStore(fieldOffsetValue, data.fieldOffset);

	// Store the class object into the provided variable
	llvm::Value *varPtr = GetVariablePointer(ctx, node->outputVariable);
	m_builder.CreateStore(classValue, varPtr);

	return {};
}

} // namespace wren_llvm_backend

// Wire it together with the public LLVMBackend class
LLVMBackend::~LLVMBackend() {}

std::unique_ptr<LLVMBackend> LLVMBackend::Create() {
	return std::unique_ptr<LLVMBackend>(new wren_llvm_backend::LLVMBackendImpl);
}

LLVMBackend *create_llvm_backend() {
	std::unique_ptr<LLVMBackend> backend = LLVMBackend::Create();
	return backend.release();
}

#endif // USE_LLVM
