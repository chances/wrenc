//
// Created by znix on 09/12/22.
//

#include "wrencc_config.h"

#ifdef USE_LLVM

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
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>

#include <fmt/format.h>

#include <map>

// Wrap everything in a namespace to avoid any possibility of name collisions
namespace wren_llvm_backend {

struct VisitorContext {
	/// For each local variable, stack memory is allocated for it (and later optimised away - we do this
	/// to avoid having to deal with SSA, and this is also how Clang does it) and the value for that
	/// stack address is stored here.
	std::map<LocalVariable *, llvm::Value *> localAddresses;

	llvm::Value *GetAddress(LocalVariable *var) const {
		const auto iter = localAddresses.find(var);
		if (iter != localAddresses.end())
			return iter->second;

		fmt::print(stderr, "Found unallocated local variable: '{}'\n", var->Name());
		abort();
	}
};

struct ExprRes {
	llvm::Value *value = nullptr;
};

struct StmtRes {};

class LLVMBackendImpl : public LLVMBackend {
  public:
	LLVMBackendImpl();

	CompilationResult Generate(Module *aModule) override;

  private:
	llvm::Function *GenerateFunc(IRFn *func, bool initialiser);
	void GenerateInitialiser();

	llvm::Constant *GetStringConst(const std::string &str);
	llvm::Value *GetManagedStringValue(const std::string &str);
	llvm::Value *GetGlobalVariable(IRGlobalDecl *global);

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
	llvm::FunctionCallee m_createClosure;

	llvm::PointerType *m_pointerType = nullptr;
	llvm::Type *m_signatureType = nullptr;
	llvm::IntegerType *m_valueType = nullptr;
	llvm::IntegerType *m_int32Type = nullptr;
	llvm::StructType *m_closureSpecType = nullptr;

	llvm::ConstantInt *m_nullValue = nullptr;
	llvm::Constant *m_nullPointer = nullptr;

	std::map<std::string, llvm::GlobalVariable *> m_systemVars;
	std::map<std::string, llvm::GlobalVariable *> m_stringConstants;
	std::map<std::string, llvm::GlobalVariable *> m_managedStrings;
	std::map<IRGlobalDecl *, llvm::GlobalVariable *> m_globalVariables;

	// A set of all the system variables used in the code. Any other system variables will be removed.
	std::set<llvm::GlobalVariable *> m_usedSystemVars;

	std::map<IRFn *, llvm::Function *> m_generatedFunctions;
	std::map<IRFn *, llvm::GlobalVariable *> m_closureSpecs;
};

LLVMBackendImpl::LLVMBackendImpl() : m_builder(m_context), m_module("myModule", m_context) {
	m_valueType = llvm::Type::getInt64Ty(m_context);
	m_signatureType = llvm::Type::getInt64Ty(m_context);
	m_pointerType = llvm::PointerType::get(m_context, 0);
	m_int32Type = llvm::Type::getInt32Ty(m_context);

	m_closureSpecType = llvm::StructType::get(m_context, {m_pointerType, m_pointerType, m_int32Type, m_int32Type});

	m_nullValue = llvm::ConstantInt::get(m_valueType, encode_object(nullptr));
	m_nullPointer = llvm::ConstantPointerNull::get(m_pointerType);

	std::vector<llvm::Type *> fnLookupArgs = {m_valueType, m_signatureType};
	llvm::FunctionType *fnLookupType = llvm::FunctionType::get(m_pointerType, fnLookupArgs, false);
	m_virtualMethodLookup = m_module.getOrInsertFunction("wren_virtual_method_lookup", fnLookupType);

	std::vector<llvm::Type *> newClosureArgs = {m_pointerType, m_pointerType, m_pointerType};
	llvm::FunctionType *newClosureTypes = llvm::FunctionType::get(m_valueType, newClosureArgs, false);
	m_createClosure = m_module.getOrInsertFunction("wren_create_closure", newClosureTypes);
}

CompilationResult LLVMBackendImpl::Generate(Module *module) {
	// Create all the system variables with the correct linkage
	for (const auto &entry : ExprSystemVar::SYSTEM_VAR_NAMES) {
		std::string name = "wren_sys_var_" + entry.first;
		m_systemVars[entry.first] = new llvm::GlobalVariable(m_module, m_valueType, false,
		                                                     llvm::GlobalVariable::InternalLinkage, m_nullValue, name);
	}

	llvm::FunctionType *initFuncType = llvm::FunctionType::get(llvm::Type::getVoidTy(m_context), false);
	m_initFunc = llvm::Function::Create(initFuncType, llvm::Function::PrivateLinkage, "module_init", &m_module);

	for (IRFn *func : module->GetClosures()) {
		// Make a global variable for the ClosureSpec
		m_closureSpecs[func] =
		    new llvm::GlobalVariable(m_module, m_pointerType, false, llvm::GlobalVariable::InternalLinkage,
		                             m_nullPointer, "spec_" + func->debugName);
	}

	for (IRFn *func : module->GetFunctions()) {
		m_generatedFunctions[func] = GenerateFunc(func, func == module->GetMainFunction());
	}

	// Generate the initialiser last, when we know all the string constants etc
	GenerateInitialiser();

	// Identify this module as containing the main function - TODO with defineStandaloneMainFunc variable
	if (true) {
		// Emit a pointer to the main module function. This is picked up by the stub the programme gets linked to.
		// This stub (in rtsrc/standalone_main_stub.cpp) uses the OS's standard crti/crtn and similar objects to
		// make a working executable, and it'll load this pointer when we link this object to it.
		// Also, put it in .data not .rodata since it contains a relocation.
		// Const-casating is ugly, but it works.
		llvm::Function *main = m_generatedFunctions.at(const_cast<IRFn *>(module->GetMainFunction()));
		new llvm::GlobalVariable(m_module, m_pointerType, true, llvm::GlobalVariable::ExternalLinkage, main,
		                         "wrenStandaloneMainFunc");
	}

	// Test it
	m_module.print(llvm::outs(), nullptr);

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
	std::vector<llvm::Type *> funcArgs(func->arity, m_valueType);
	llvm::FunctionType *ft = llvm::FunctionType::get(m_valueType, funcArgs, false);

	llvm::Function *function = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, func->debugName, &m_module);
	llvm::BasicBlock *bb = llvm::BasicBlock::Create(m_context, "entry", function);
	m_builder.SetInsertPoint(bb);

	if (initialiser) {
		// Call the initialiser, which we'll generate later
		m_builder.CreateCall(m_initFunc->getFunctionType(), m_initFunc);
	}

	VisitorContext ctx = {};

	for (LocalVariable *local : func->locals) {
		ctx.localAddresses[local] = m_builder.CreateAlloca(m_valueType, nullptr, local->Name());
	}
	for (LocalVariable *local : func->temporaries) {
		ctx.localAddresses[local] = m_builder.CreateAlloca(m_valueType, nullptr, local->Name());
	}

	VisitStmt(&ctx, func->body);

	return function;
}

void LLVMBackendImpl::GenerateInitialiser() {
	llvm::BasicBlock *bb = llvm::BasicBlock::Create(m_context, "entry", m_initFunc);
	m_builder.SetInsertPoint(bb);

	// Remove any unused system variables, for ease of reading the LLVM IR
	for (const auto &entry : ExprSystemVar::SYSTEM_VAR_NAMES) {
		llvm::GlobalVariable *var = m_systemVars.at(entry.first);
		if (m_usedSystemVars.contains(var))
			continue;
		m_systemVars.erase(entry.first);
		m_module.getGlobalList().erase(var);
	}

	// Load the variables for all the core values
	std::vector<llvm::Type *> argTypes = {m_pointerType};
	llvm::FunctionType *sysLookupType = llvm::FunctionType::get(m_valueType, argTypes, false);
	llvm::FunctionCallee getSysVarFn = m_module.getOrInsertFunction("wren_get_core_class_value", sysLookupType);

	for (const auto &entry : m_systemVars) {
		std::vector<llvm::Value *> args = {GetStringConst(entry.first)};
		llvm::Value *result = m_builder.CreateCall(getSysVarFn, args, "var_" + entry.first);

		m_builder.CreateStore(result, entry.second);
	}

	// Create all the string constants

	argTypes = {m_pointerType, m_int32Type};
	llvm::FunctionType *newStringType = llvm::FunctionType::get(m_valueType, argTypes, false);
	llvm::FunctionCallee newStringFn = m_module.getOrInsertFunction("wren_init_string_literal", newStringType);

	for (const auto &entry : m_managedStrings) {
		// Create a raw C string
		llvm::Constant *strPtr = GetStringConst(entry.first);

		// And construct a string object from it
		std::vector<llvm::Value *> args = {strPtr, llvm::ConstantInt::get(m_int32Type, entry.first.size())};
		llvm::Value *value = m_builder.CreateCall(newStringFn, args);

		m_builder.CreateStore(value, entry.second);
	}

	// Register the upvalues, creating ClosureSpec-s for each closure
	argTypes = {m_pointerType};
	llvm::FunctionType *newClosureType = llvm::FunctionType::get(m_pointerType, argTypes, false);
	llvm::FunctionCallee newClosureFn = m_module.getOrInsertFunction("wren_register_closure", newClosureType);
	for (const auto &entry : m_closureSpecs) {
		// For each upvalue, tell the runtime about it and save the description object it gives us. This object
		// is then used to closure objects wrapping this function.

		llvm::Function *impl = m_generatedFunctions.at(entry.first);

		// Generate the spec table
		std::vector<llvm::Constant *> structContent = {
		    impl,                                   // function pointer
		    GetStringConst(entry.first->debugName), // name C string
		    llvm::ConstantInt::get(m_int32Type, 0), // Arity
		    llvm::ConstantInt::get(m_int32Type, 0), // Upvalue count
		};
		llvm::Constant *constant = llvm::ConstantStruct::get(m_closureSpecType, structContent);

		llvm::GlobalVariable *specData =
		    new llvm::GlobalVariable(m_module, constant->getType(), true, llvm::GlobalVariable::PrivateLinkage,
		                             constant, "closure_spec_" + entry.first->debugName);

		// And generate the registration code
		std::vector<llvm::Value *> args = {specData};
		llvm::Value *spec = m_builder.CreateCall(newClosureFn, args, entry.first->debugName);
		m_builder.CreateStore(spec, entry.second);
	}

	// Functions must return!
	m_builder.CreateRetVoid();
}

llvm::Constant *LLVMBackendImpl::GetStringConst(const std::string &str) {
	auto iter = m_stringConstants.find(str);
	if (iter != m_stringConstants.end())
		return iter->second;

	std::vector<int8_t> data(str.size() + 1);
	std::copy(str.begin(), str.end(), data.begin());

	llvm::Constant *constant = llvm::ConstantDataArray::get(m_context, data);
	llvm::GlobalVariable *var = new llvm::GlobalVariable(m_module, constant->getType(), true,
	                                                     llvm::GlobalVariable::PrivateLinkage, constant, "str_" + str);

	m_stringConstants[str] = var;
	return var;
}

llvm::Value *LLVMBackendImpl::GetManagedStringValue(const std::string &str) {
	auto iter = m_managedStrings.find(str);
	if (iter != m_managedStrings.end())
		return iter->second;

	llvm::GlobalVariable *var = new llvm::GlobalVariable(
	    m_module, m_valueType, false, llvm::GlobalVariable::PrivateLinkage, m_nullValue, "strobj_" + str);
	m_managedStrings[str] = var;
	return var;
}

llvm::Value *LLVMBackendImpl::GetGlobalVariable(IRGlobalDecl *global) {
	auto iter = m_globalVariables.find(global);
	if (iter != m_globalVariables.end())
		return iter->second;

	llvm::GlobalVariable *var = new llvm::GlobalVariable(
	    m_module, m_valueType, false, llvm::GlobalVariable::PrivateLinkage, m_nullValue, "gbl_" + global->Name());
	m_globalVariables[global] = var;
	return var;
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
		// FIXME only use a short prefix of the string
		value = m_builder.CreateLoad(m_valueType, ptr, "strobj_" + node->value.s);
		break;
	}
	case CcValue::BOOL:
		abort();
		break;
	case CcValue::INT:
		value = llvm::ConstantInt::get(m_valueType, encode_number(node->value.i));
		break;
	case CcValue::NUM:
		value = llvm::ConstantInt::get(m_valueType, encode_number(node->value.n));
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
		llvm::Value *ptr = ctx->GetAddress(local);
		llvm::Value *value = m_builder.CreateLoad(m_valueType, ptr, node->var->Name() + "_value");
		return {value};
	} else if (upvalue) {
		fprintf(stderr, "TODO load upvalue\n");
		abort();
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
	printf("error: not implemented: VisitExprFieldLoad\n");
	abort();
	return {};
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
	// TODO put in signature list
	SignatureId signature = hash_util::findSignatureId(name);
	llvm::Value *sigValue = llvm::ConstantInt::get(m_signatureType, signature.id);

	// Call the lookup function
	std::vector<llvm::Value *> lookupArgs = {receiver.value, sigValue};
	llvm::CallInst *func = m_builder.CreateCall(m_virtualMethodLookup, lookupArgs, "vptr_" + name);

	// Make the function type - TODO cache
	std::vector<llvm::Type *> argTypes(args.size(), m_valueType);
	llvm::FunctionType *type = llvm::FunctionType::get(m_valueType, argTypes, false);

	// Invoke it
	llvm::CallInst *result = m_builder.CreateCall(type, func, args);

	return {result};
}
ExprRes LLVMBackendImpl::VisitExprClosure(VisitorContext *ctx, ExprClosure *node) {
	if (!node->func->upvalues.empty()) {
		printf("error: not implemented: VisitExprClosure with upvalues\n");
		abort();
	}

	llvm::Value *specObj =
	    m_builder.CreateLoad(m_pointerType, m_closureSpecs.at(node->func), "closure_spec_" + node->func->debugName);
	std::vector<llvm::Value *> args = {specObj, m_nullPointer, m_nullPointer};
	llvm::Value *closure = m_builder.CreateCall(m_createClosure, args, "closure_" + node->func->debugName);

	return {closure};
}
ExprRes LLVMBackendImpl::VisitExprLoadReceiver(VisitorContext *ctx, ExprLoadReceiver *node) {
	printf("error: not implemented: VisitExprLoadReceiver\n");
	abort();
	return {};
}
ExprRes LLVMBackendImpl::VisitExprRunStatements(VisitorContext *ctx, ExprRunStatements *node) {
	VisitStmt(ctx, node->statement);

	llvm::Value *ptr = ctx->GetAddress(node->temporary);
	llvm::Value *value = m_builder.CreateLoad(m_valueType, ptr, "temp_value");

	return {value};
}
ExprRes LLVMBackendImpl::VisitExprAllocateInstanceMemory(VisitorContext *ctx, ExprAllocateInstanceMemory *node) {
	printf("error: not implemented: VisitExprAllocateInstanceMemory\n");
	abort();
	return {};
}
ExprRes LLVMBackendImpl::VisitExprSystemVar(VisitorContext *ctx, ExprSystemVar *node) {
	llvm::GlobalVariable *global = m_systemVars.at(node->name);
	m_usedSystemVars.insert(global);
	llvm::Value *value = m_builder.CreateLoad(m_valueType, global, "gbl_" + node->name);
	return {value};
}
ExprRes LLVMBackendImpl::VisitExprGetClassVar(VisitorContext *ctx, ExprGetClassVar *node) {
	printf("error: not implemented: VisitExprGetClassVar\n");
	abort();
	return {};
}

// Statements

StmtRes LLVMBackendImpl::VisitStmtAssign(VisitorContext *ctx, StmtAssign *node) {
	LocalVariable *local = dynamic_cast<LocalVariable *>(node->var);
	UpvalueVariable *upvalue = dynamic_cast<UpvalueVariable *>(node->var);
	IRGlobalDecl *global = dynamic_cast<IRGlobalDecl *>(node->var);

	llvm::Value *value = VisitExpr(ctx, node->expr).value;

	if (local) {
		llvm::Value *ptr = ctx->GetAddress(local);
		m_builder.CreateStore(value, ptr);
	} else if (upvalue) {
		fprintf(stderr, "TODO assign upvalue\n");
		abort();
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
	printf("error: not implemented: VisitStmtFieldAssign\n");
	abort();
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
	printf("error: not implemented: VisitStmtLabel\n");
	abort();
	return {};
}
StmtRes LLVMBackendImpl::VisitStmtJump(VisitorContext *ctx, StmtJump *node) {
	printf("error: not implemented: VisitStmtJump\n");
	abort();
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
	printf("error: not implemented: VisitStmtRelocateUpvalues\n");
	abort();
	return {};
}

} // namespace wren_llvm_backend

// Wire it together with the public LLVMBackend class
LLVMBackend::~LLVMBackend() {}

std::unique_ptr<LLVMBackend> LLVMBackend::Create() {
	return std::unique_ptr<LLVMBackend>(new wren_llvm_backend::LLVMBackendImpl);
}

#endif // USE_LLVM
