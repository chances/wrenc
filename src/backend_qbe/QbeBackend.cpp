//
// Created by znix on 20/07/22.
//

#include "QbeBackend.h"
#include "CompContext.h"

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

void QbeBackend::Generate(IRFn *fn) {
	VisitFn(fn);
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
#define DISPATCH_FUNC(type, func)                                                                                      \
	do {                                                                                                               \
		if (typeid(*expr) == typeid(type))                                                                             \
			return func(dynamic_cast<type *>(expr));                                                                   \
	} while (0)
#define DISPATCH(type) DISPATCH_FUNC(type, Visit##type)

	DISPATCH(ExprConst);
	DISPATCH(ExprLoad);
	DISPATCH(ExprFieldLoad);
	DISPATCH(ExprFuncCall);
	DISPATCH(ExprClosure);
	DISPATCH(ExprLoadReceiver);
	DISPATCH(ExprRunStatements);
	DISPATCH(ExprLogicalNot);
	DISPATCH(ExprAllocateInstanceMemory);
	DISPATCH(ExprSystemVar);

#undef DISPATCH
#undef DISPATCH_FUNC

	fmt::print("Unknown expr node {}\n", typeid(*expr).name());
	return nullptr;
}

QbeBackend::Snippet *QbeBackend::VisitStmt(IRStmt *expr) {
#define DISPATCH_FUNC(type, func)                                                                                      \
	do {                                                                                                               \
		if (typeid(*expr) == typeid(type))                                                                             \
			return func(dynamic_cast<type *>(expr));                                                                   \
	} while (0)
#define DISPATCH(type) DISPATCH_FUNC(type, Visit##type)

	DISPATCH(StmtAssign);
	DISPATCH(StmtFieldAssign);
	DISPATCH_FUNC(StmtUpvalueImport, VisitStmtUpvalue);
	DISPATCH(StmtEvalAndIgnore);
	DISPATCH_FUNC(StmtBlock, VisitBlock);
	DISPATCH(StmtLabel);
	DISPATCH(StmtJump);
	DISPATCH(StmtReturn);
	DISPATCH(StmtLoadModule);

#undef DISPATCH
#undef DISPATCH_FUNC

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
	// TODO we can optimise this a lot
	// TODO arguments
	// TODO use signature ID rather than stringifying it
	std::string signature = GetStringPtr(node->signature->ToString());
	snip->Add("%{} ={} call $wren_virtual_dispatch({} %{}, {} {})", snip->result->name, PTR_TYPE, PTR_TYPE,
	          receiver->name, PTR_TYPE, signature);
	return snip;
}

QbeBackend::Snippet *QbeBackend::VisitExprSystemVar(ExprSystemVar *node) {
	Snippet *snip = m_alloc.New<Snippet>();
	snip->result = AddTemporary("sys_var_" + node->name);
	// Pass IDs instead of strings
	int id = ExprSystemVar::SYSTEM_VAR_NAMES.at(node->name);
	snip->Add("%{} ={} call $wren_get_sys_var({} {})", snip->result->name, PTR_TYPE, PTR_TYPE, id);
	return snip;
}

QbeBackend::Snippet *QbeBackend::VisitStmtReturn(StmtReturn *node) {
	Snippet *snip = m_alloc.New<Snippet>();
	VLocal *result = snip->Add(VisitExpr(node->value));
	snip->Add("ret %{}", result->name);
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
	const int MAX_SIZE = 30;
	if (symbolName.size() > MAX_SIZE)
		symbolName.resize(MAX_SIZE);
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
QbeBackend::Snippet *QbeBackend::VisitExprLoad(ExprLoad *node) { return HandleUnimplemented(node); }
QbeBackend::Snippet *QbeBackend::VisitExprFieldLoad(ExprFieldLoad *node) { return HandleUnimplemented(node); }
QbeBackend::Snippet *QbeBackend::VisitExprClosure(ExprClosure *node) { return HandleUnimplemented(node); }
QbeBackend::Snippet *QbeBackend::VisitExprLoadReceiver(ExprLoadReceiver *node) { return HandleUnimplemented(node); }
QbeBackend::Snippet *QbeBackend::VisitExprRunStatements(ExprRunStatements *node) { return HandleUnimplemented(node); }
QbeBackend::Snippet *QbeBackend::VisitExprLogicalNot(ExprLogicalNot *node) { return HandleUnimplemented(node); }
QbeBackend::Snippet *QbeBackend::VisitExprAllocateInstanceMemory(ExprAllocateInstanceMemory *node) {
	return HandleUnimplemented(node);
}
