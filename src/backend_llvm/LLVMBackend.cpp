//
// Created by znix on 09/12/22.
//

#include "wrencc_config.h"

#ifdef USE_LLVM

#include "CompContext.h"
#include "HashUtil.h"
#include "LLVMBackend.h"
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

struct VisitorContext {};

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

	llvm::FunctionCallee m_virtualMethodLookup;

	llvm::Type *m_pointerType = nullptr;
	llvm::Type *m_signatureType = nullptr;
	llvm::IntegerType *m_valueType = nullptr;

	llvm::ConstantInt *m_nullValue = nullptr;

	std::map<std::string, llvm::GlobalVariable *> m_systemVars;
	std::map<std::string, llvm::GlobalVariable *> m_stringConstants;
};

LLVMBackendImpl::LLVMBackendImpl() : m_builder(m_context), m_module("myModule", m_context) {
	m_valueType = llvm::Type::getInt64Ty(m_context);
	m_signatureType = llvm::Type::getInt64Ty(m_context);
	m_pointerType = llvm::PointerType::get(m_context, 0);

	m_nullValue = llvm::ConstantInt::get(m_valueType, encode_object(nullptr));

	std::vector<llvm::Type *> fnLookupArgs = {m_valueType, m_signatureType};
	llvm::FunctionType *fnLookupType = llvm::FunctionType::get(m_pointerType, fnLookupArgs, false);
	m_virtualMethodLookup = m_module.getOrInsertFunction("wren_virtual_method_lookup", fnLookupType);
}

CompilationResult LLVMBackendImpl::Generate(Module *module) {
	// Create all the system variables with the correct linkage
	for (const auto &entry : ExprSystemVar::SYSTEM_VAR_NAMES) {
		std::string name = "wren_sys_var_" + entry.first;
		m_systemVars[entry.first] = new llvm::GlobalVariable(m_module, m_valueType, false,
		                                                     llvm::GlobalVariable::InternalLinkage, m_nullValue, name);
	}

	std::map<std::string, llvm::Function *> functions;
	for (IRFn *func : module->GetFunctions()) {
		functions[func->debugName] = GenerateFunc(func, func == module->GetMainFunction());
	}

	// Identify this module as containing the main function - TODO with defineStandaloneMainFunc variable
	if (true) {
		// Emit a pointer to the main module function. This is picked up by the stub the programme gets linked to.
		// This stub (in rtsrc/standalone_main_stub.cpp) uses the OS's standard crti/crtn and similar objects to
		// make a working executable, and it'll load this pointer when we link this object to it.
		// Also, put it in .data not .rodata since it contains a relocation.
		llvm::Function *main = functions.at(module->GetMainFunction()->debugName);
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
	llvm::Optional<llvm::Reloc::Model> relocModel;
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
		GenerateInitialiser();
	}

	VisitorContext ctx = {};
	VisitStmt(&ctx, func->body);

	return function;
}

void LLVMBackendImpl::GenerateInitialiser() {
	std::vector<llvm::Type *> argTypes = {};
	llvm::FunctionType *sysLookupType = llvm::FunctionType::get(m_valueType, argTypes, false);
	llvm::FunctionCallee getSysVarFn = m_module.getOrInsertFunction("wren_get_core_class_value", sysLookupType);

	for (const auto &entry : ExprSystemVar::SYSTEM_VAR_NAMES) {
		std::vector<llvm::Value *> args = {GetStringConst(entry.first)};
		llvm::Value *result = m_builder.CreateCall(getSysVarFn, args, "var_" + entry.first);

		llvm::GlobalVariable *dest = m_systemVars[entry.first];
		m_builder.CreateStore(result, dest);
	}
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
	case CcValue::STRING:
		abort();
		break;
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
	printf("error: not implemented: VisitExprLoad\n");
	abort();
	return {};
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
	printf("error: not implemented: VisitExprClosure\n");
	abort();
	return {};
}
ExprRes LLVMBackendImpl::VisitExprLoadReceiver(VisitorContext *ctx, ExprLoadReceiver *node) {
	printf("error: not implemented: VisitExprLoadReceiver\n");
	abort();
	return {};
}
ExprRes LLVMBackendImpl::VisitExprRunStatements(VisitorContext *ctx, ExprRunStatements *node) {
	printf("error: not implemented: VisitExprRunStatements\n");
	abort();
	return {};
}
ExprRes LLVMBackendImpl::VisitExprAllocateInstanceMemory(VisitorContext *ctx, ExprAllocateInstanceMemory *node) {
	printf("error: not implemented: VisitExprAllocateInstanceMemory\n");
	abort();
	return {};
}
ExprRes LLVMBackendImpl::VisitExprSystemVar(VisitorContext *ctx, ExprSystemVar *node) {
	llvm::Value *global = m_systemVars.at(node->name);
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
	printf("error: not implemented: VisitStmtAssign\n");
	abort();
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
