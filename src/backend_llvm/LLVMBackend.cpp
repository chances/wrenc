//
// Created by znix on 09/12/22.
//

#include "wrencc_config.h"

#ifdef USE_LLVM

#include "ClassDescription.h"
#include "ClassInfo.h"
#include "CompContext.h"
#include "HashUtil.h"
#include "LLVMBackend.h"
#include "Scope.h"
#include "common.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/ModRef.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>

#include <fmt/format.h>

#include <map>

using CInt = llvm::ConstantInt;

// Wrap everything in a namespace to avoid any possibility of name collisions
namespace wren_llvm_backend {

struct UpvaluePackDef {
	// All the variables bound to upvalues in the relevant closure
	std::vector<UpvalueVariable *> variables;

	// The positions of the variables in the upvalue pack, the inverse of variables
	std::unordered_map<UpvalueVariable *, int> variableIds;
};

struct VisitorContext {
	/// For each local variable, stack memory is allocated for it (and later optimised away - we do this
	/// to avoid having to deal with SSA, and this is also how Clang does it) and the value for that
	/// stack address is stored here.
	///
	/// This does not contain entries for variables used by closures.
	std::map<LocalVariable *, llvm::Value *> localAddresses;

	/// For each variable that some closure uses, they're stored in a single large array. This contains
	/// the position of each of them in that array.
	std::map<LocalVariable *, int> closedAddressPositions;

	/// The array of closable variables
	llvm::Value *closableVariables = nullptr;

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

	/// Same meaning as VisitorContext.closedAddressPositions
	std::map<LocalVariable *, int> closedAddressPositions;
};

class ClassData : public BackendNodeData {
  public:
	llvm::GlobalVariable *object = nullptr;
	llvm::GlobalVariable *fieldOffset = nullptr;
};

class LLVMBackendImpl : public LLVMBackend {
  public:
	LLVMBackendImpl();

	CompilationResult Generate(Module *mod) override;

  private:
	llvm::Function *GenerateFunc(IRFn *func, bool initialiser);
	void GenerateInitialiser(Module *mod);

	llvm::Constant *GetStringConst(const std::string &str);
	llvm::Value *GetManagedStringValue(const std::string &str);
	llvm::GlobalVariable *GetGlobalVariable(IRGlobalDecl *global);
	llvm::Value *GetLocalPointer(VisitorContext *ctx, LocalVariable *local);
	llvm::Value *GetUpvaluePointer(VisitorContext *ctx, UpvalueVariable *upvalue);
	llvm::BasicBlock *GetLabelBlock(VisitorContext *ctx, StmtLabel *label);

	/// Process a string literal, to make it suitable for using as a name in the IR
	static std::string FilterStringLiteral(const std::string &literal);

	ExprRes VisitExpr(VisitorContext *ctx, IRExpr *expr);
	StmtRes VisitStmt(VisitorContext *ctx, IRStmt *expr);

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
	StmtRes VisitStmtRelocateUpvalues(VisitorContext *ctx, StmtRelocateUpvalues *node);

	llvm::LLVMContext m_context;
	llvm::IRBuilder<> m_builder;
	llvm::Module m_module;

	llvm::Function *m_initFunc = nullptr;

	llvm::FunctionCallee m_virtualMethodLookup;
	llvm::FunctionCallee m_superMethodLookup;
	llvm::FunctionCallee m_createClosure;
	llvm::FunctionCallee m_allocUpvalueStorage;
	llvm::FunctionCallee m_getUpvaluePack;
	llvm::FunctionCallee m_getNextClosure;
	llvm::FunctionCallee m_allocObject;
	llvm::FunctionCallee m_initClass;
	llvm::FunctionCallee m_getClassFieldOffset;
	llvm::FunctionCallee m_registerSignatureTable;

	llvm::PointerType *m_pointerType = nullptr;
	llvm::Type *m_signatureType = nullptr;
	llvm::IntegerType *m_valueType = nullptr;
	llvm::IntegerType *m_int8Type = nullptr;
	llvm::IntegerType *m_int32Type = nullptr;
	llvm::IntegerType *m_int64Type = nullptr;
	llvm::Type *m_voidType = nullptr;

	llvm::ConstantInt *m_nullValue = nullptr;
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
};

LLVMBackendImpl::LLVMBackendImpl() : m_builder(m_context), m_module("myModule", m_context) {
	m_valueType = llvm::Type::getInt64Ty(m_context);
	m_signatureType = llvm::Type::getInt64Ty(m_context);
	m_pointerType = llvm::PointerType::get(m_context, 0);
	m_int8Type = llvm::Type::getInt8Ty(m_context);
	m_int32Type = llvm::Type::getInt32Ty(m_context);
	m_int64Type = llvm::Type::getInt64Ty(m_context);
	m_voidType = llvm::Type::getVoidTy(m_context);

	m_nullValue = CInt::get(m_valueType, encode_object(nullptr));
	m_nullPointer = llvm::ConstantPointerNull::get(m_pointerType);

	std::vector<llvm::Type *> fnLookupArgs = {m_valueType, m_signatureType};
	llvm::FunctionType *fnLookupType = llvm::FunctionType::get(m_pointerType, fnLookupArgs, false);
	m_virtualMethodLookup = m_module.getOrInsertFunction("wren_virtual_method_lookup", fnLookupType);

	llvm::FunctionType *superLookupType =
	    llvm::FunctionType::get(m_pointerType, {m_valueType, m_valueType, m_int64Type, m_int8Type}, false);
	m_superMethodLookup = m_module.getOrInsertFunction("wren_super_method_lookup", superLookupType);

	std::vector<llvm::Type *> newClosureArgs = {m_pointerType, m_pointerType, m_pointerType, m_pointerType};
	llvm::FunctionType *newClosureTypes = llvm::FunctionType::get(m_valueType, newClosureArgs, false);
	m_createClosure = m_module.getOrInsertFunction("wren_create_closure", newClosureTypes);

	llvm::FunctionType *allocUpvalueStorageType = llvm::FunctionType::get(m_pointerType, {m_int32Type}, false);
	m_allocUpvalueStorage = m_module.getOrInsertFunction("wren_alloc_upvalue_storage", allocUpvalueStorageType);

	llvm::FunctionType *getUpvaluePackType = llvm::FunctionType::get(m_pointerType, {m_pointerType}, false);
	m_getUpvaluePack = m_module.getOrInsertFunction("wren_get_closure_upvalue_pack", getUpvaluePackType);

	llvm::FunctionType *getNextClosureType = llvm::FunctionType::get(m_pointerType, {m_pointerType}, false);
	m_getNextClosure = m_module.getOrInsertFunction("wren_get_closure_chain_next", getNextClosureType);

	llvm::FunctionType *allocObjectType = llvm::FunctionType::get(m_valueType, {m_valueType}, false);
	m_allocObject = m_module.getOrInsertFunction("wren_alloc_obj", allocObjectType);

	llvm::FunctionType *initClassType =
	    llvm::FunctionType::get(m_valueType, {m_pointerType, m_pointerType, m_valueType}, false);
	m_initClass = m_module.getOrInsertFunction("wren_init_class", initClassType);

	llvm::FunctionType *getFieldOffsetType = llvm::FunctionType::get(m_int32Type, {m_valueType}, false);
	m_getClassFieldOffset = m_module.getOrInsertFunction("wren_class_get_field_offset", getFieldOffsetType);

	llvm::FunctionType *registerSigTableType = llvm::FunctionType::get(m_voidType, {m_pointerType}, false);
	m_registerSignatureTable = m_module.getOrInsertFunction("wren_register_signatures_table", registerSigTableType);
}

CompilationResult LLVMBackendImpl::Generate(Module *mod) {
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
		func->backendData = std::unique_ptr<BackendNodeData>(new FnData);
	}

	for (IRFn *func : mod->GetClosures()) {
		// Make a global variable for the ClosureSpec
		FnData *fnData = func->GetBackendData<FnData>();
		fnData->closureSpec =
		    new llvm::GlobalVariable(m_module, m_pointerType, false, llvm::GlobalVariable::InternalLinkage,
		                             m_nullPointer, "spec_" + func->debugName);

		// Make the upvalue pack for each function that needs them
		std::unique_ptr<UpvaluePackDef> pack = std::make_unique<UpvaluePackDef>();

		for (const auto &entry : func->upvalues) {
			// Assign an increasing series of IDs for each variable in an arbitrary order
			pack->variables.push_back(entry.second);
			pack->variableIds[entry.second] = pack->variables.size() - 1; // -1 to get the index of the last entry
		}

		// Note we always have to register an upvalue pack definition, even if it's empty - it's required for closures.
		fnData->upvaluePackDef = std::move(pack);
	}

	// Add pointers to the ObjClass instances for all the classes we've declared
	for (IRClass *cls : mod->GetClasses()) {
		// System classes are defined in C++, and should only appear when compiling wren_core
		if (cls->info->IsSystemClass())
			continue;

		llvm::GlobalVariable *classObj =
		    new llvm::GlobalVariable(m_module, m_valueType, false, llvm::GlobalVariable::InternalLinkage, m_nullValue,
		                             "class_obj_" + cls->info->name);

		// Add fields to store the offset of the member fields in each class. This is required since we
		// can currently have classes extending from classes in another file, and they don't know how
		// many fields said classes have. Thus this field will get loaded at startup as a byte offset and
		// we have to add it to the object pointer to get the fields area.
		llvm::GlobalVariable *memberOffset =
		    new llvm::GlobalVariable(m_module, m_int32Type, false, llvm::GlobalVariable::InternalLinkage,
		                             CInt::get(m_int32Type, 0), "class_field_offset_" + cls->info->name);

		ClassData *classData = new ClassData;
		classData->object = classObj;
		classData->fieldOffset = memberOffset;
		cls->backendData = std::unique_ptr<BackendNodeData>(classData);
	}

	for (IRFn *func : mod->GetFunctions()) {
		FnData *data = func->GetBackendData<FnData>();
		data->llvmFunc = GenerateFunc(func, func == mod->GetMainFunction());
	}

	// Generate the initialiser last, when we know all the string constants etc
	GenerateInitialiser(mod);

	// Identify this module as containing the main function
	if (defineStandaloneMainFunc) {
		// Emit a pointer to the main module function. This is picked up by the stub the programme gets linked to.
		// This stub (in rtsrc/standalone_main_stub.cpp) uses the OS's standard crti/crtn and similar objects to
		// make a working executable, and it'll load this pointer when we link this object to it.
		// Also, put it in .data not .rodata since it contains a relocation.
		FnData *data = mod->GetMainFunction()->GetBackendData<FnData>();
		llvm::Function *main = data->llvmFunc;
		new llvm::GlobalVariable(m_module, m_pointerType, true, llvm::GlobalVariable::ExternalLinkage, main,
		                         "wrenStandaloneMainFunc");
	}

	// Print out the LLVM IR for debugging
	const char *dumpIrStr = getenv("DUMP_LLVM_IR");
	if (dumpIrStr && dumpIrStr == std::string("1")) {
		m_module.print(llvm::outs(), nullptr);
	}

	// Verify the IR, to make sure we haven't done something strange
	if (llvm::verifyModule(m_module, &llvm::outs())) {
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
	std::optional<llvm::Reloc::Model> relocModel;
	llvm::TargetMachine *targetMachine = target->createTargetMachine(targetTriple, cpu, features, opt, relocModel);

	m_module.setDataLayout(targetMachine->createDataLayout());
	m_module.setTargetTriple(targetTriple);

	// Actually generate the code
	std::string filename = "/tmp/wren-output.o";
	std::error_code ec;
	llvm::raw_fd_ostream dest(filename, ec, llvm::sys::fs::OF_None);

	if (ec) {
		std::string msg = ec.message();
		fprintf(stderr, "Could not open file: %s", msg.c_str());
		exit(1);
	}

	// TODO switch to the new PassManager
	llvm::legacy::PassManager pass;
	llvm::CodeGenFileType fileType = llvm::CGFT_ObjectFile;

	if (targetMachine->addPassesToEmitFile(pass, dest, nullptr, fileType)) {
		fprintf(stderr, "TargetMachine can't emit a file of this type");
		exit(1);
	}

	pass.run(m_module);
	dest.flush();

	return CompilationResult{
	    .successful = true,
	    .tempFilename = filename,
	    .format = CompilationResult::OBJECT,
	};
}

llvm::Function *LLVMBackendImpl::GenerateFunc(IRFn *func, bool initialiser) {
	FnData *fnData = func->GetBackendData<FnData>();

	// Only take an upvalue pack argument if we actually need it
	bool takesUpvaluePack = fnData->upvaluePackDef && !fnData->upvaluePackDef->variables.empty();

	// Set up the function arguments
	std::vector<llvm::Type *> funcArgs;
	if (func->enclosingClass) {
		// The receiver ('this') value.
		funcArgs.push_back(m_valueType);
	}
	if (takesUpvaluePack) {
		// If this function uses upvalues, they're passed as an argument
		funcArgs.push_back(m_pointerType);
	}

	// The 'regular' arguments, that the user would see
	funcArgs.insert(funcArgs.end(), func->parameters.size(), m_valueType);

	llvm::FunctionType *ft = llvm::FunctionType::get(m_valueType, funcArgs, false);

	llvm::Function *function = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, func->debugName, &m_module);
	llvm::BasicBlock *bb = llvm::BasicBlock::Create(m_context, "entry", function);
	m_builder.SetInsertPoint(bb);

	if (initialiser) {
		// Call the initialiser, which we'll generate later
		m_builder.CreateCall(m_initFunc->getFunctionType(), m_initFunc);
	}

	VisitorContext ctx = {};
	ctx.currentFunc = function;
	ctx.currentWrenFunc = func;

	std::vector<LocalVariable *> closables;

	for (LocalVariable *local : func->locals) {
		if (local->upvalues.empty()) {
			// Normal local variable
			ctx.localAddresses[local] = m_builder.CreateAlloca(m_valueType, nullptr, local->Name());
		} else {
			// This variable is accessed by closures, so it gets stored in the array of closable variables.
			ctx.closedAddressPositions[local] = closables.size();
			closables.push_back(local);
		}
	}
	for (LocalVariable *local : func->temporaries) {
		ctx.localAddresses[local] = m_builder.CreateAlloca(m_valueType, nullptr, local->Name());
	}

	if (!closables.empty()) {
		ctx.closableVariables =
		    m_builder.CreateCall(m_allocUpvalueStorage, {CInt::get(m_int32Type, closables.size())}, "upvalueStorage");
	}

	// Copy across the position data, as it's used to generate the closure specs
	fnData->closedAddressPositions = ctx.closedAddressPositions;

	// Load the upvalue pack
	int nextArg = 0;
	if (func->enclosingClass) {
		ctx.receiver = function->getArg(nextArg++);
		ctx.receiver->setName("this");

		// Find a pointer to this object, since the 'this' argument is a value.
		// We'll just assume it's always a pointer, since there's no valid reason why it could be a number.
		// (note this is still an i64, we'll convert it to a pointer later)
		llvm::Value *thisPtr = m_builder.CreateAnd(ctx.receiver, CInt::get(m_int64Type, CONTENT_MASK));

		// Also find the pointer to our field block
		// We don't know the position of the class's fields at compile time, they're stored in a variable that's
		// loaded during class initialisation. Use this as an offset into our object to find where the fields start.
		// We only need this if we access fields, but that's quite common and LLVM should optimise it out if
		// it's not needed

		ClassData *cd = func->enclosingClass->GetBackendData<ClassData>();
		llvm::Value *fieldStartOffset = m_builder.CreateLoad(m_int32Type, cd->fieldOffset, "this_field_offset");
		llvm::Value *wideStartOffset =
		    m_builder.CreateIntCast(fieldStartOffset, m_int64Type, false, "this_field_offset_64");

		llvm::Value *fieldPtrInt = m_builder.CreateAdd(thisPtr, wideStartOffset, "fields_ptr_int");
		ctx.fieldPointer = m_builder.CreateIntToPtr(fieldPtrInt, m_pointerType, "fields_ptr");
	}
	if (takesUpvaluePack) {
		ctx.upvaluePack = fnData->upvaluePackDef.get();
		ctx.upvaluePackPtr = function->getArg(nextArg++);
		ctx.upvaluePackPtr->setName("upvalue_pack");
	}
	for (LocalVariable *arg : func->parameters) {
		// Load the arguments
		llvm::Value *destPtr = GetLocalPointer(&ctx, arg);
		llvm::Value *value = function->getArg(nextArg++);
		value->setName(arg->Name());
		m_builder.CreateStore(value, destPtr);
	}

	VisitStmt(&ctx, func->body);

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

	argTypes = {m_pointerType, m_int32Type};
	llvm::FunctionType *newStringType = llvm::FunctionType::get(m_valueType, argTypes, false);
	llvm::FunctionCallee newStringFn = m_module.getOrInsertFunction("wren_init_string_literal", newStringType);

	for (const auto &entry : m_managedStrings) {
		// Create a raw C string
		llvm::Constant *strPtr = GetStringConst(entry.first);

		// And construct a string object from it
		std::vector<llvm::Value *> args = {strPtr, CInt::get(m_int32Type, entry.first.size())};
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

		std::vector<llvm::Type *> specTypes = {m_pointerType, m_pointerType, m_int32Type, m_int32Type};
		specTypes.insert(specTypes.end(), numUpvalues, m_int32Type); // Add the upvalue indices
		llvm::StructType *closureSpecType = llvm::StructType::get(m_context, specTypes);

		// Generate the spec table
		std::vector<llvm::Constant *> structContent = {
		    fnData->llvmFunc,                              // function pointer
		    GetStringConst(fn->debugName),                 // name C string
		    CInt::get(m_int32Type, fn->parameters.size()), // Arity
		    CInt::get(m_int32Type, numUpvalues),           // Upvalue count
		};
		for (UpvalueVariable *upvalue : upvaluePack->variables) {
			IRFn *parentFn = fn->parent;
			FnData *parentData = parentFn->GetBackendData<FnData>();

			LocalVariable *target = dynamic_cast<LocalVariable *>(upvalue->parent);
			if (!target) {
				// This upvalue must point to another upvalue, right?
				UpvalueVariable *nested = dynamic_cast<UpvalueVariable *>(upvalue->parent);
				if (!nested) {
					fmt::print(stderr, "Upvalue {} has non-local, non-upvalue parent scope {}.\n", upvalue->Name(),
					           upvalue->parent->Scope());
					abort();
				}

				// This upvalue should belong to our parent function.
				assert(nested->containingFunction == parentFn);

				// Then the value is already stored on the heap. Find the parent's upvalue's position in the parent
				// function's upvalue pack. The parent function is the one that calls wren_create_closure, and passes
				// in it's upvalue pack. That's why we're using the parent function's upvalue pack.
				int index = parentData->upvaluePackDef->variableIds.at(nested);

				// Set the MSB (when truncated to an i32) to indicate the value comes from
				// the upvalue pack passed into this function, rather than the one it allocates.
				index |= 1ull << 31;
				structContent.push_back(CInt::get(m_int32Type, index));
				continue;
			}

			if (!parentData->closedAddressPositions.contains(target)) {
				fmt::print(stderr, "Function {} doesn't have closeable local {}, used by closure {}.\n",
				           parentFn->debugName, target->Name(), fn->debugName);
				abort();
			}
			int index = parentData->closedAddressPositions.at(target);

			structContent.push_back(CInt::get(m_int32Type, index));
		}

		llvm::Constant *constant = llvm::ConstantStruct::get(closureSpecType, structContent);

		llvm::GlobalVariable *specData =
		    new llvm::GlobalVariable(m_module, constant->getType(), true, llvm::GlobalVariable::PrivateLinkage,
		                             constant, "closure_spec_" + fn->debugName);

		// And generate the registration code
		std::vector<llvm::Value *> args = {specData};
		llvm::Value *spec = m_builder.CreateCall(newClosureFn, args, fn->debugName);
		m_builder.CreateStore(spec, fnData->closureSpec);
	}

	// Load the Obj type once, since we'll likely use it a lot since it's the default supertype
	llvm::Value *objValue = m_builder.CreateLoad(m_valueType, m_systemVars.at("Object"), "obj_class");

	for (IRClass *cls : mod->GetClasses()) {
		using CD = ClassDescription;
		using Cmd = ClassDescription::Command;

		const ClassData &data = *cls->GetBackendData<ClassData>();

		// Build the data as a list of i64s
		std::vector<llvm::Constant *> values;

		auto addCmdFlag = [this, &values](Cmd cmd, int flags) {
			// This is the same (with little-endian) as two i32s: <ADD_METHOD> <flags>
			uint64_t cmdFlag = (uint64_t)cmd + ((uint64_t)flags << 32);
			values.push_back(CInt::get(m_int64Type, cmdFlag));
		};

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
				addCmdFlag(Cmd::ADD_METHOD, flags);

				llvm::Constant *strConst = GetStringConst(sig->ToString());
				values.push_back(llvm::ConstantExpr::getPtrToInt(strConst, m_int64Type));

				FnData *fnData = method->fn->GetBackendData<FnData>();
				values.push_back(llvm::ConstantExpr::getPtrToInt(fnData->llvmFunc, m_int64Type));

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
		llvm::GlobalVariable *classDataBlock =
		    new llvm::GlobalVariable(m_module, dataBlockType, true, llvm::GlobalVariable::PrivateLinkage, dataConstant,
		                             "class_data_" + cls->info->name);

		// Inherits from Object by default
		llvm::Value *supertype = objValue;
		if (cls->info->parentClass) {
			// Be a bit limiting here, and only allow explicitly inheriting from other classes in the same module
			// So for example, this won't work:
			//   class A { }
			//   var B = A
			//   class C is B {}
			// TODO improve this

			ExprLoad *classVar = dynamic_cast<ExprLoad *>(cls->info->parentClass);
			if (!classVar) {
				fmt::print(stderr, "Complicated superclasses for class {} are not yet supported in the LLVM backend.\n",
				           cls->info->name);
				abort();
			}

			IRGlobalDecl *global = dynamic_cast<IRGlobalDecl *>(classVar->var);
			if (!global) {
				fmt::print(stderr, "Cannot use a local variable as the supertype of class {}\n", cls->info->name);
				abort();
			}

			IRClass *superclass = global->targetClass;
			if (!superclass) {
				fmt::print(stderr, "Class {} cannot extend a variable that is not statically known to be a class\n",
				           cls->info->name);
				abort();
			}

			ClassData *supertypeData = superclass->GetBackendData<ClassData>();
			supertype = m_builder.CreateLoad(m_valueType, supertypeData->object, "supertype_" + superclass->info->name);
		}

		llvm::Constant *className = GetStringConst(cls->info->name);
		llvm::Value *classValue = m_builder.CreateCall(m_initClass, {className, classDataBlock, supertype});
		m_builder.CreateStore(classValue, data.object);

		// Look up the field offset
		llvm::Value *fieldOffsetValue =
		    m_builder.CreateCall(m_getClassFieldOffset, {classValue}, "field_offset_" + cls->info->name);
		m_builder.CreateStore(fieldOffsetValue, data.fieldOffset);
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
		llvm::GlobalVariable *var = new llvm::GlobalVariable(
		    m_module, constant->getType(), true, llvm::GlobalVariable::PrivateLinkage, constant, "signatures_table");
		m_builder.CreateCall(m_registerSignatureTable, {var});
	}

	// Functions must return!
	m_builder.CreateRetVoid();

	// Finally, generate the get-global-table function, if this module has a name - otherwise it can't be imported
	// anyway. The name here is important, as it's part of the ABI.
	if (mod->Name()) {
		std::string getGlobalName = mod->Name().value() + "_get_globals";
		llvm::FunctionType *getGblFuncType = llvm::FunctionType::get(m_pointerType, false);
		llvm::Function *getGlobalsFunc =
		    llvm::Function::Create(getGblFuncType, llvm::Function::ExternalLinkage, getGlobalName, &m_module);
		m_builder.SetInsertPoint(llvm::BasicBlock::Create(m_context, "entry", getGlobalsFunc));

		// Create the globals table
		std::vector<llvm::Constant *> components;
		for (IRGlobalDecl *global : mod->GetGlobalVariables()) {
			components.push_back(GetStringConst(global->Name()));
			components.push_back(GetGlobalVariable(global));
		}
		// Terminating null
		components.push_back(m_nullPointer);

		llvm::ArrayType *arrayType = llvm::ArrayType::get(m_pointerType, components.size());
		llvm::Constant *value = llvm::ConstantArray::get(arrayType, components);
		llvm::GlobalVariable *globalsTable = new llvm::GlobalVariable(
		    m_module, arrayType, true, llvm::GlobalVariable::PrivateLinkage, value, "globals_table");

		m_builder.CreateRet(globalsTable);
	}
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

	llvm::GlobalVariable *var = new llvm::GlobalVariable(
	    m_module, m_valueType, false, llvm::GlobalVariable::PrivateLinkage, m_nullValue, "gbl_" + global->Name());
	m_globalVariables[global] = var;
	return var;
}

llvm::Value *LLVMBackendImpl::GetLocalPointer(VisitorContext *ctx, LocalVariable *local) {
	const auto iter = ctx->localAddresses.find(local);
	if (iter != ctx->localAddresses.end())
		return iter->second;

	// Check if it's a closed-over variable
	const auto iter2 = ctx->closedAddressPositions.find(local);
	if (iter2 != ctx->closedAddressPositions.end()) {
		std::vector<llvm::Value *> indices = {
		    // Select the item we're interested in.
		    CInt::get(m_int32Type, iter2->second),
		};
		return m_builder.CreateGEP(m_valueType, ctx->closableVariables, indices, "lv_ptr_" + local->Name());
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

llvm::BasicBlock *LLVMBackendImpl::GetLabelBlock(VisitorContext *ctx, StmtLabel *label) {
	if (ctx->labelBlocks.contains(label))
		return ctx->labelBlocks.at(label);

	// Don't supply the function - we'll add it when we find the StmtLabel, so it'll be in the proper order
	// when read manually. The order of the BBs shouldn't matter (AFAIK) to the compiler.
	llvm::BasicBlock *block = llvm::BasicBlock::Create(m_context, "lbl_" + label->debugName);

	ctx->labelBlocks[label] = block;
	return block;
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

// Visitors
ExprRes LLVMBackendImpl::VisitExpr(VisitorContext *ctx, IRExpr *expr) {
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
	DISPATCH(VisitStmtRelocateUpvalues, StmtRelocateUpvalues);

#undef DISPATCH

	fmt::print("Unknown stmt node {}\n", typeid(*expr).name());
	abort();
	return {};
}

ExprRes LLVMBackendImpl::VisitExprConst(VisitorContext *ctx, ExprConst *node) {
	llvm::Value *value;
	switch (node->value.type) {
	case CcValue::NULL_TYPE:
		value = m_nullValue;
		break;
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
		value = CInt::get(m_valueType, encode_number(node->value.i));
		break;
	case CcValue::NUM:
		value = CInt::get(m_valueType, encode_number(node->value.n));
		break;
	default:
		fprintf(stderr, "Invalid constant node type %d\n", (int)node->value.type);
		abort();
		break;
	}

	return {value};
}
ExprRes LLVMBackendImpl::VisitExprLoad(VisitorContext *ctx, ExprLoad *node) {
	LocalVariable *local = dynamic_cast<LocalVariable *>(node->var);
	UpvalueVariable *upvalue = dynamic_cast<UpvalueVariable *>(node->var);
	IRGlobalDecl *global = dynamic_cast<IRGlobalDecl *>(node->var);

	if (local) {
		llvm::Value *ptr = GetLocalPointer(ctx, local);
		llvm::Value *value = m_builder.CreateLoad(m_valueType, ptr, node->var->Name() + "_value");
		return {value};
	} else if (upvalue) {
		llvm::Value *ptr = GetUpvaluePointer(ctx, upvalue);
		llvm::Value *var = m_builder.CreateLoad(m_valueType, ptr, "uv_" + upvalue->Name());
		return {var};
	} else if (global) {
		llvm::Value *ptr = GetGlobalVariable(global);
		llvm::Value *value = m_builder.CreateLoad(m_valueType, ptr, node->var->Name() + "_value");
		return {value};
	} else {
		fprintf(stderr, "Attempted to load non-local, non-global, non-upvalue variable %p\n", node->var);
		abort();
	}
}
ExprRes LLVMBackendImpl::VisitExprFieldLoad(VisitorContext *ctx, ExprFieldLoad *node) {
	llvm::Value *fieldPointer = m_builder.CreateGEP(
	    m_valueType, ctx->fieldPointer, {CInt::get(m_int32Type, node->var->Id())}, "field_ptr_" + node->var->Name());

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
	if (!node->super) {
		std::vector<llvm::Value *> lookupArgs = {receiver.value, sigValue};
		func = m_builder.CreateCall(m_virtualMethodLookup, lookupArgs, "vptr_" + name);
	} else {
		// Super lookups are special - we pass in the class this method is declared on, so the lookup function
		// can find the correct method.
		IRClass *cls = ctx->currentWrenFunc->enclosingClass;
		if (!cls) {
			fmt::print(stderr, "error: Found super call to {} from {}, which is not a method!\n", name,
			           ctx->currentWrenFunc->debugName);
			abort();
		}

		if (dynamic_cast<ExprLoadReceiver *>(node->receiver) == nullptr) {
			fmt::print(stderr, "error: Found super call to {} from {}, on non-ExprLoadReceiver receiver!\n", name,
			           ctx->currentWrenFunc->debugName);
			abort();
		}

		llvm::GlobalVariable *classVar = cls->GetBackendData<ClassData>()->object;
		llvm::Value *thisClass = m_builder.CreateLoad(m_valueType, classVar, "super_cls_" + cls->info->name);

		llvm::Value *isStatic = CInt::get(m_int8Type, ctx->currentWrenFunc->methodInfo->isStatic);

		std::vector<llvm::Value *> lookupArgs = {receiver.value, thisClass, sigValue, isStatic};
		func = m_builder.CreateCall(m_superMethodLookup, lookupArgs, "vptr_" + name);
	}

	// Make the function type - TODO cache
	std::vector<llvm::Type *> argTypes(args.size(), m_valueType);
	llvm::FunctionType *type = llvm::FunctionType::get(m_valueType, argTypes, false);

	// Invoke it
	llvm::CallInst *result = m_builder.CreateCall(type, func, args);

	return {result};
}
ExprRes LLVMBackendImpl::VisitExprClosure(VisitorContext *ctx, ExprClosure *node) {
	llvm::Value *closables = m_nullPointer;
	llvm::Value *upvalueTable = m_nullPointer;
	if (!node->func->upvalues.empty()) {
		closables = ctx->closableVariables;
	}
	if (ctx->upvaluePackPtr) {
		upvalueTable = ctx->upvaluePackPtr;
	}

	llvm::Value *closureSpec = node->func->GetBackendData<FnData>()->closureSpec;
	llvm::Value *specObj = m_builder.CreateLoad(m_pointerType, closureSpec, "closure_spec_" + node->func->debugName);
	std::vector<llvm::Value *> args = {specObj, closables, upvalueTable, m_nullPointer};
	llvm::Value *closure = m_builder.CreateCall(m_createClosure, args, "closure_" + node->func->debugName);

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
	LocalVariable *local = dynamic_cast<LocalVariable *>(node->var);
	UpvalueVariable *upvalue = dynamic_cast<UpvalueVariable *>(node->var);
	IRGlobalDecl *global = dynamic_cast<IRGlobalDecl *>(node->var);

	llvm::Value *value = VisitExpr(ctx, node->expr).value;

	if (local) {
		llvm::Value *ptr = GetLocalPointer(ctx, local);
		m_builder.CreateStore(value, ptr);
	} else if (upvalue) {
		llvm::Value *ptr = GetUpvaluePointer(ctx, upvalue);
		m_builder.CreateStore(value, ptr);
	} else if (global) {
		llvm::Value *ptr = GetGlobalVariable(global);
		m_builder.CreateStore(value, ptr);
	} else {
		fprintf(stderr, "Attempted to store to non-local, non-global, non-upvalue variable %p\n", node->var);
		abort();
	}

	return {};
}
StmtRes LLVMBackendImpl::VisitStmtFieldAssign(VisitorContext *ctx, StmtFieldAssign *node) {
	ExprRes res = VisitExpr(ctx, node->value);

	llvm::Value *fieldPointer = m_builder.CreateGEP(
	    m_valueType, ctx->fieldPointer, {CInt::get(m_int32Type, node->var->Id())}, "field_ptr_" + node->var->Name());

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
	printf("error: not implemented: VisitStmtLoadModule\n");
	abort();
	return {};
}
StmtRes LLVMBackendImpl::VisitStmtRelocateUpvalues(VisitorContext *ctx, StmtRelocateUpvalues *node) {
	// All upvalues in the LLVM implementation are stored on the heap, since things get too complicated with
	// nested closures otherwise. Thus (until we implement a GC) we don't have to do anything here.

	return {};
}

} // namespace wren_llvm_backend

// Wire it together with the public LLVMBackend class
LLVMBackend::~LLVMBackend() {}

std::unique_ptr<LLVMBackend> LLVMBackend::Create() {
	return std::unique_ptr<LLVMBackend>(new wren_llvm_backend::LLVMBackendImpl);
}

#endif // USE_LLVM
