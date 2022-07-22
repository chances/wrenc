//
// Created by znix on 20/07/22.
//

#include "QbeBackend.h"
#include "CompContext.h"

#include "ObjClass.h"

#include <Scope.h>
#include <SymbolTable.h>
#include <fmt/format.h>
#include <fstream>
#include <sstream>

#define ASSERT(cond, msg)                                                                                              \
	do {                                                                                                               \
		bool assert_macro_cond = (cond);                                                                               \
		if (!assert_macro_cond)                                                                                        \
			assertionFailure(__FILE__, __LINE__, (msg));                                                               \
	} while (0)

static void assertionFailure(const char *file, int line, const char *msg) {
	fmt::print("Assertion failure at {}:{} - {}\n", file, line, msg);
	abort();
}

QbeBackend::QbeBackend() = default;
QbeBackend::~QbeBackend() = default;

void QbeBackend::Generate(Module *module) {
	for (IRFn *func : module->GetFunctions()) {
		VisitFn(func);
	}

	// Add the global variables
	for (IRGlobalDecl *var : module->GetGlobalVariables()) {
		Print("section \".data\" data {} = {{ {} {} }}", MangleGlobalName(var), PTR_TYPE, (uint64_t)NULL_VAL);
	}

	// Add the strings
	for (const auto &[literal, name] : m_strings) {
		std::string escaped = literal;
		for (int i = 0; i < escaped.size(); i++) {
			char c = escaped.at(i);
			if (c != '"' && c != '\\')
				continue;

			// Insert an escape string
			escaped.insert(escaped.begin() + i, '\\');

			// Skip the character, otherwise we'll keep escaping it in an infinite loop
			i++;
		}
		Print("section \".rodata\" data {} = {{b \"{}\", b 0}}", name, escaped);
	}

	// TODO only for main module:
	// Emit a pointer to the main module function. This is picked up by the stub the programme gets linked to.
	// This stub (in rtsrc/standalone_main_stub.cpp) uses the OS's standard crti/crtn and similar objects to
	// make a working executable, and it'll load this pointer when we link this object to it.
	std::string mainFuncName = MangleUniqueName(module->GetMainFunction()->debugName);
	Print("section \".rodata\" export data $wrenStandaloneMainFunc = {{ {} ${} }}", PTR_TYPE, mainFuncName);

	fmt::print("Generated QBE IR:\n{}\n", m_output.str());

	std::ofstream output;
	output.exceptions(std::ios::badbit | std::ios::failbit);
	try {
		output.open("/tmp/wren_qbe_ir.qbe");
		output << m_output.str() << std::endl;
	} catch (const std::fstream::failure &ex) {
		fmt::print(stderr, "Failed to write QBE IR: {}\n", ex.what());
		exit(1);
	}
}

template <typename... Args> void QbeBackend::Print(fmt::format_string<Args...> fmtStr, Args &&...args) {
	std::string formatted = fmt::format(std::move(fmtStr), std::forward<Args>(args)...);

	if (m_inFunction) {
		m_output << "\t";
	}

	m_output << formatted << std::endl;
}

template <typename... Args> void QbeBackend::Snippet::Add(fmt::format_string<Args...> fmtStr, Args &&...args) {
	std::string formatted = fmt::format(std::move(fmtStr), std::forward<Args>(args)...);
	lines.push_back(formatted);
}

QbeBackend::VLocal *QbeBackend::Snippet::Add(Snippet *other) {
	// Copy over the lines, and return the result. It's fine for the caller to ignore the result, QBE will optimise
	// it away in that case.
	lines.insert(lines.end(), std::make_move_iterator(other->lines.begin()),
	             std::make_move_iterator(other->lines.end()));
	return other->result;
}

// Visitor functions //

QbeBackend::Snippet *QbeBackend::VisitExpr(IRExpr *expr) {
#define DISPATCH(func, type)                                                                                           \
	do {                                                                                                               \
		if (typeid(*expr) == typeid(type))                                                                             \
			return func(dynamic_cast<type *>(expr));                                                                   \
	} while (0)

	// Use both the function name and type for ease of searching and IDE indexing
	DISPATCH(VisitExprConst, ExprConst);
	DISPATCH(VisitExprLoad, ExprLoad);
	DISPATCH(VisitExprFieldLoad, ExprFieldLoad);
	DISPATCH(VisitExprFuncCall, ExprFuncCall);
	DISPATCH(VisitExprClosure, ExprClosure);
	DISPATCH(VisitExprLoadReceiver, ExprLoadReceiver);
	DISPATCH(VisitExprRunStatements, ExprRunStatements);
	DISPATCH(VisitExprLogicalNot, ExprLogicalNot);
	DISPATCH(VisitExprAllocateInstanceMemory, ExprAllocateInstanceMemory);
	DISPATCH(VisitExprSystemVar, ExprSystemVar);

#undef DISPATCH

	fmt::print("Unknown expr node {}\n", typeid(*expr).name());
	return nullptr;
}

QbeBackend::Snippet *QbeBackend::VisitStmt(IRStmt *expr) {
#define DISPATCH(func, type)                                                                                           \
	do {                                                                                                               \
		if (typeid(*expr) == typeid(type))                                                                             \
			return func(dynamic_cast<type *>(expr));                                                                   \
	} while (0)

	DISPATCH(VisitStmtAssign, StmtAssign);
	DISPATCH(VisitStmtFieldAssign, StmtFieldAssign);
	DISPATCH(VisitStmtUpvalue, StmtUpvalueImport);
	DISPATCH(VisitStmtEvalAndIgnore, StmtEvalAndIgnore);
	DISPATCH(VisitBlock, StmtBlock);
	DISPATCH(VisitStmtLabel, StmtLabel);
	DISPATCH(VisitStmtJump, StmtJump);
	DISPATCH(VisitStmtReturn, StmtReturn);
	DISPATCH(VisitStmtLoadModule, StmtLoadModule);

#undef DISPATCH

	fmt::print("Unknown expr node {}\n", typeid(*expr).name());
	return nullptr;
}

void QbeBackend::VisitFn(IRFn *node) {
	// TODO argument handling
	Print("function {} ${}() {{", PTR_TYPE, MangleUniqueName(node->debugName));
	Print("@start");
	m_inFunction = true;

	StmtBlock *block = dynamic_cast<StmtBlock *>(node->body);
	ASSERT(block, "Function body must be a StmtBlock");
	for (IRStmt *stmt : block->statements) {
		Snippet *snip = VisitStmt(stmt);
		for (const std::string &line : snip->lines) {
			Print("{}", line);
		}
	}

	m_inFunction = false;
	Print("}}");
}

QbeBackend::Snippet *QbeBackend::VisitStmtAssign(StmtAssign *node) {
	Snippet *snip = m_alloc.New<Snippet>();

	VLocal *input = snip->Add(VisitExpr(node->expr));

	LocalVariable *local = dynamic_cast<LocalVariable *>(node->var);
	IRGlobalDecl *global = dynamic_cast<IRGlobalDecl *>(node->var);
	if (local) {
		snip->Add("%{} ={} copy %{}", LookupVariable(local)->name, PTR_TYPE, input->name);
	} else if (global) {
		snip->Add("store{} %{}, {}", PTR_TYPE, input->name, MangleGlobalName(global));
	} else {
		fmt::print(stderr, "Unknown variable type in assignment: {}\n", typeid(*node->var).name());
	}

	return snip;
}

QbeBackend::Snippet *QbeBackend::VisitExprConst(ExprConst *node) {
	Snippet *snip = m_alloc.New<Snippet>();
	VLocal *tmp = AddTemporary("const_value");
	snip->Add("%{} ={} copy {}", tmp->name, PTR_TYPE, node->value.ToRuntimeValue());
	snip->result = tmp;
	return snip;
}

QbeBackend::Snippet *QbeBackend::VisitStmtEvalAndIgnore(StmtEvalAndIgnore *node) {
	// Ignore the result, how much simpler could it get :)
	return VisitExpr(node->expr);
}

QbeBackend::Snippet *QbeBackend::VisitExprFuncCall(ExprFuncCall *node) {
	Snippet *snip = m_alloc.New<Snippet>();
	VLocal *receiver = snip->Add(VisitExpr(node->receiver));
	snip->result = AddTemporary("func_call_result");

	// First, lookup the address of the function
	// TODO inline the fast-case lookup code (check one entry slot)
	VLocal *funcPtr = AddTemporary("vfunc_ptr");
	// TODO we can optimise this a lot
	std::string signatureStr = node->signature->ToString();
	uint64_t signature = ObjClass::FindSignatureId(signatureStr);
	// Print the signature string as a comment to aid manually reading IR
	snip->Add("%{} ={} call $wren_virtual_method_lookup({} %{}, l {}) # {}", funcPtr->name, PTR_TYPE, PTR_TYPE,
	          receiver->name, signature, signatureStr);

	// Emit all the arguments
	std::string argStr = fmt::format("{} %{}, ", PTR_TYPE, receiver->name);
	for (IRExpr *argExpr : node->args) {
		VLocal *result = snip->Add(VisitExpr(argExpr));
		argStr += fmt::format("{} %{}, ", PTR_TYPE, result->name);
	}

	// Produce the actual method call
	snip->Add("%{} ={} call %{}({})", snip->result->name, PTR_TYPE, funcPtr->name, argStr);
	return snip;
}

QbeBackend::Snippet *QbeBackend::VisitExprSystemVar(ExprSystemVar *node) {
	Snippet *snip = m_alloc.New<Snippet>();
	snip->result = AddTemporary("sys_var_" + node->name);
	snip->Add("%{} ={} load{} $wren_sys_var_{}", snip->result->name, PTR_TYPE, PTR_TYPE, node->name);
	return snip;
}

QbeBackend::Snippet *QbeBackend::VisitStmtReturn(StmtReturn *node) {
	Snippet *snip = m_alloc.New<Snippet>();
	VLocal *result = snip->Add(VisitExpr(node->value));
	snip->Add("ret %{}", result->name);
	return snip;
}

QbeBackend::Snippet *QbeBackend::VisitExprLoad(ExprLoad *node) {
	LocalVariable *local = dynamic_cast<LocalVariable *>(node->var);
	IRGlobalDecl *global = dynamic_cast<IRGlobalDecl *>(node->var);
	Snippet *snip = m_alloc.New<Snippet>();
	snip->result = AddTemporary("var_load_" + MangleUniqueName(node->var->Name()));
	if (local) {
		snip->Add("%{} ={} copy %{}", snip->result->name, PTR_TYPE, LookupVariable(local)->name);
	} else if (global) {
		snip->Add("%{} ={} load{} {}", snip->result->name, PTR_TYPE, PTR_TYPE, MangleGlobalName(global));
	} else {
		fmt::print(stderr, "Unknown variable type in load: {}\n", typeid(*node->var).name());
	}
	return snip;
}

// Utility functions //

QbeBackend::Snippet *QbeBackend::HandleUnimplemented(IRNode *node) {
	fmt::print(stderr, "QbeBackend: Unsupported node {}\n", typeid(*node).name());
	return nullptr;
}

QbeBackend::VLocal *QbeBackend::AddTemporary(std::string debugName) {
	m_temporaries.push_back(std::make_unique<VLocal>());
	VLocal *local = m_temporaries.back().get();
	local->name = MangleUniqueName(debugName);
	return local;
}

std::string QbeBackend::GetStringPtr(const std::string &value) {
	auto iter = m_strings.find(value);
	if (iter != m_strings.end())
		return iter->second;

	// TODO make this a valid and unique symbol
	std::string symbolName = "$str_" + std::to_string(m_strings.size()) + "_" + MangleRawName(value, true);
	const int MAX_SIZE = 35;
	const std::string TRUNCATION_SUFFIX = "_TRUNC";
	if (symbolName.size() > MAX_SIZE) {
		symbolName.resize(MAX_SIZE - TRUNCATION_SUFFIX.size());
		symbolName.insert(symbolName.end(), TRUNCATION_SUFFIX.begin(), TRUNCATION_SUFFIX.end());
	}
	m_strings[value] = symbolName;
	return symbolName;
}

QbeBackend::VLocal *QbeBackend::LookupVariable(LocalVariable *decl) {
	auto iter = m_locals.find(decl);
	if (iter != m_locals.end())
		return iter->second.get();

	m_locals[decl] = std::make_unique<VLocal>();
	VLocal *local = m_locals.at(decl).get();
	local->name = decl->name;
	// TODO unique-ify
	return local;
}

std::string QbeBackend::MangleGlobalName(IRGlobalDecl *var) {
	// TODO add module name etc
	return "$" + MangleRawName(var->name, false);
}

std::string QbeBackend::MangleUniqueName(const std::string &name) {
	auto iter = m_uniqueNames.find(name);
	if (iter != m_uniqueNames.end())
		return iter->second;

	std::string base = MangleRawName(name, true);

	// If required, put on a numerical suffix
	std::string mangled = base;
	int i = 1;
	while (m_uniqueNames.contains(mangled)) {
		mangled = base + "_" + std::to_string(i++);
	}

	m_uniqueNames[name] = mangled;
	return mangled;
}

std::string QbeBackend::MangleRawName(const std::string &str, bool permitAmbiguity) {
	std::string name = str;
	for (int i = 0; i < name.size(); i++) {
		char c = name.at(i);

		// Filtering ASCII alphanumerical characters, as that's all we can reliably use with QBE
		if ('0' <= c && c <= '9')
			continue;
		if ('a' <= c && c <= 'z')
			continue;
		if ('A' <= c && c <= 'Z')
			continue;

		// If some ambiguity is permitted, then leave underscores alone
		if (permitAmbiguity && c == '_')
			continue;

		// Make it an underscore, followed by it's byte value in hex
		name.at(i) = '_';
		std::string code = fmt::format("{:02x}", (uint8_t)c);
		name.insert(name.begin() + i + 1, code.begin(), code.end());

		// Skip over the characters we just added
		i += 2;
	}
	return name;
}

// Unimplemented Visitors //

QbeBackend::Snippet *QbeBackend::VisitClass(IRClass *node) { return HandleUnimplemented(node); }
QbeBackend::Snippet *QbeBackend::VisitGlobalDecl(IRGlobalDecl *node) { return HandleUnimplemented(node); }
QbeBackend::Snippet *QbeBackend::VisitImport(IRImport *node) { return HandleUnimplemented(node); }
QbeBackend::Snippet *QbeBackend::VisitStmtFieldAssign(StmtFieldAssign *node) { return HandleUnimplemented(node); }
QbeBackend::Snippet *QbeBackend::VisitStmtUpvalue(StmtUpvalueImport *node) { return HandleUnimplemented(node); }
QbeBackend::Snippet *QbeBackend::VisitBlock(StmtBlock *node) { return HandleUnimplemented(node); }
QbeBackend::Snippet *QbeBackend::VisitStmtLabel(StmtLabel *node) { return HandleUnimplemented(node); }
QbeBackend::Snippet *QbeBackend::VisitStmtJump(StmtJump *node) { return HandleUnimplemented(node); }
QbeBackend::Snippet *QbeBackend::VisitStmtLoadModule(StmtLoadModule *node) { return HandleUnimplemented(node); }
QbeBackend::Snippet *QbeBackend::VisitExprFieldLoad(ExprFieldLoad *node) { return HandleUnimplemented(node); }
QbeBackend::Snippet *QbeBackend::VisitExprClosure(ExprClosure *node) { return HandleUnimplemented(node); }
QbeBackend::Snippet *QbeBackend::VisitExprLoadReceiver(ExprLoadReceiver *node) { return HandleUnimplemented(node); }
QbeBackend::Snippet *QbeBackend::VisitExprRunStatements(ExprRunStatements *node) { return HandleUnimplemented(node); }
QbeBackend::Snippet *QbeBackend::VisitExprLogicalNot(ExprLogicalNot *node) { return HandleUnimplemented(node); }
QbeBackend::Snippet *QbeBackend::VisitExprAllocateInstanceMemory(ExprAllocateInstanceMemory *node) {
	return HandleUnimplemented(node);
}
