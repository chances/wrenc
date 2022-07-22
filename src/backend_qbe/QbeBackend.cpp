//
// Created by znix on 20/07/22.
//

#include "QbeBackend.h"
#include "ClassDescription.h"
#include "ClassInfo.h"
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
	std::string moduleName = MangleUniqueName(module->Name().value_or("unknown"), false);

	for (IRFn *func : module->GetFunctions()) {
		std::optional<std::string> initFunc;
		if (func == module->GetMainFunction()) {
			initFunc = "module_init_func_" + moduleName;
		}
		VisitFn(func, initFunc);
	}

	// Generate the initialisation function
	GenerateInitFunction(moduleName, module);

	// Add the global variables
	for (IRGlobalDecl *var : module->GetGlobalVariables()) {
		Print("section \".data\" data {} = {{ {} {} }}", MangleGlobalName(var), PTR_TYPE, (uint64_t)NULL_VAL);
	}

	// Add a table of data describing each class, what their methods are etc
	// This works as a series of commands with flags
	// Note we create new strings here, so this has to be done before writing the strings
	for (IRClass *cls : module->GetClasses()) {
		using CD = ClassDescription;
		using Cmd = ClassDescription::Command;

		Print("section \".data\" data $class_desc_{} = {{", cls->info->name);

		// Emit a block for each method
		auto writeMethods = [this](const ClassInfo::MethodMap &methods, bool isStatic) {
			for (const auto &[sig, method] : methods) {
				int flags = 0;
				if (isStatic)
					flags |= CD::FLAG_STATIC;
				Print("w {} {},", (int)Cmd::ADD_METHOD, flags);

				std::string sigSym = GetStringPtr(sig->ToString());
				Print("{} {},", PTR_TYPE, sigSym);
				Print("{} ${},", PTR_TYPE, m_functionNames.at(method->fn));
			}
		};

		writeMethods(cls->info->methods, false);
		writeMethods(cls->info->staticMethods, true);

		Print("w {} 0,", (int)Cmd::END, CD::FLAG_NONE); // Signal the end of the class description
		Print("}}");
	}

	// Add the strings
	for (const auto &[literal, name] : m_strings) {
		std::string escaped = EscapeString(literal);
		Print("section \".rodata\" data {} = {{b \"{}\", b 0}}", name, escaped);
	}

	// And pointers for the ObjString objects wrapping them - these are initialised when the
	// module is first loaded.
	for (const auto &[_, name] : m_stringObjs) {
		Print("section \".data\" data {} = {{ l {} }}", name, NULL_VAL);
	}

	// Add pointers to the ObjClass instances for all the classes we've declared
	for (IRClass *cls : module->GetClasses()) {
		Print("section \".data\" data $class_var_{} = {{ l {} }}", cls->info->name, NULL_VAL);
	}

	// Write the signature table - it's used for error messages, when the runtime only knows the
	// hash of a given signature.
	Print("section \".rodata\" data $signatures_table_{} = {{", moduleName);
	for (const std::string &signature : m_signatures) {
		Print("\tb \"{}\", b 0,", EscapeString(signature));
	}
	Print("b 0 }} # End with repeated zero");

	// TODO only for main module:
	// Emit a pointer to the main module function. This is picked up by the stub the programme gets linked to.
	// This stub (in rtsrc/standalone_main_stub.cpp) uses the OS's standard crti/crtn and similar objects to
	// make a working executable, and it'll load this pointer when we link this object to it.
	std::string mainFuncName = MangleUniqueName(module->GetMainFunction()->debugName, false);
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

	// Don't indent labels
	if (m_inFunction && formatted.at(0) != '@') {
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

void QbeBackend::GenerateInitFunction(const std::string &moduleName, Module *module) {
	Print("function $module_init_func_{}() {{", moduleName);
	m_inFunction = true;
	Print("@start");

	Print("# Initialise string literals");
	int i = 0;
	for (const auto &[literal, name] : m_stringObjs) {
		std::string varName = fmt::format("value_{}", i++);
		std::string storageName = name;
		storageName.erase(1, 4); // Remove the obj_ prefix
		Print("%{} =l call $wren_init_string_literal({} {}, w {})", varName, PTR_TYPE, storageName, literal.size());
		Print("storel %{}, {}", varName, name);
	}

	Print("# Register classes");
	for (IRClass *cls : module->GetClasses()) {
		std::string varName = fmt::format("tmp_class_{}", cls->info->name);
		std::string classNameSym = GetStringPtr(cls->info->name);
		Print("%{} =l call $wren_init_class({} {}, {} $class_desc_{})", varName, PTR_TYPE, classNameSym, PTR_TYPE,
		      cls->info->name);
		Print("storel %{}, $class_var_{}", varName, cls->info->name);
	}

	Print("# Register signatures table");
	Print("call $wren_register_signatures_table({} $signatures_table_{})", PTR_TYPE, moduleName);

	Print("ret");
	m_inFunction = false;
	Print("}}");
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
	DISPATCH(VisitExprGetClassVar, ExprGetClassVar);

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

	fmt::print("Unknown stmt node {}\n", typeid(*expr).name());
	return nullptr;
}

void QbeBackend::VisitFn(IRFn *node, std::optional<std::string> initFunction) {
	std::string funcName = MangleUniqueName(node->debugName, false);

	std::string args;
	if (node->isMethod) {
		// Add the receiver
		args += "l %this, ";
	}
	for (LocalVariable *arg : node->parameters) {
		VLocal *var = LookupVariable(arg);
		args += fmt::format("l %{}, ", var->name);
	}

	Print("function {} ${}({}) {{", PTR_TYPE, funcName, args);
	Print("@start");
	m_inFunction = true;
	m_exprIndentation = 0;
	m_functionNames[node] = funcName;

	if (initFunction) {
		Print("call ${}() # Module initialisation routine", initFunction.value());
	}

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
	snip->result = tmp;

	Value value = NULL_VAL;
	switch (node->value.type) {
	case CcValue::UNDEFINED:
		fmt::print(stderr, "Cannot convert UNDEFINED value to runtime value\n");
		abort();
		break;
	case CcValue::NULL_TYPE:
		value = encode_object(nullptr);
		break;
	case CcValue::INT:
		value = encode_number(node->value.i);
		break;
	case CcValue::NUM:
		value = encode_number(node->value.n);
		break;
	case CcValue::STRING: {
		// Load the global variable storing the value of this string
		snip->Add("%{} ={} load{} {}", tmp->name, PTR_TYPE, PTR_TYPE, GetStringObjPtr(node->value.s));
		return snip;
	}
	case CcValue::BOOL:
		snip->Add("%{} ={} load{} {}", tmp->name, PTR_TYPE, PTR_TYPE,
		          node->value.b ? "$wren_sys_bool_true" : "$wren_sys_bool_false");
		return snip;
	default:
		abort();
	}

	snip->Add("%{} ={} copy {}", tmp->name, PTR_TYPE, value);
	return snip;
}

QbeBackend::Snippet *QbeBackend::VisitStmtEvalAndIgnore(StmtEvalAndIgnore *node) {
	// Make sure we don't have a statement needlessly hidden inside here, though
	if (dynamic_cast<ExprRunStatements *>(node->expr)) {
		fmt::print(
		    stderr,
		    "ExprRunStatements must not appear directly inside a StmtEvalAndIgnore, it should have been cleaned up!");
	}

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
	m_signatures.insert(signatureStr);
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

QbeBackend::Snippet *QbeBackend::VisitExprGetClassVar(ExprGetClassVar *node) {
	Snippet *snip = m_alloc.New<Snippet>();
	snip->result = AddTemporary("class_" + node->name);
	snip->Add("%{} =l loadl $class_var_{}", snip->result->name, node->name);
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
	snip->result = AddTemporary("var_load_" + node->var->Name());
	if (local) {
		snip->Add("%{} ={} copy %{}", snip->result->name, PTR_TYPE, LookupVariable(local)->name);
	} else if (global) {
		snip->Add("%{} ={} load{} {}", snip->result->name, PTR_TYPE, PTR_TYPE, MangleGlobalName(global));
	} else {
		fmt::print(stderr, "Unknown variable type in load: {}\n", typeid(*node->var).name());
	}
	return snip;
}

QbeBackend::Snippet *QbeBackend::VisitExprRunStatements(ExprRunStatements *node) {
	Snippet *snip = m_alloc.New<Snippet>();

	// Instead of implementing StmtBlock, we're handling it explicitly where it's used
	// to avoid any surprises.
	StmtBlock *block = dynamic_cast<StmtBlock *>(node->statement);
	if (block) {
		for (IRStmt *stmt : block->statements)
			snip->Add(VisitStmt(stmt));
	} else {
		snip->Add(VisitStmt(node->statement));
	}
	snip->result = LookupVariable(node->temporary);

	// Add a layer of indentation to make the assembly easier to read, to make it obvious this is just computing
	// something for later use.
	for (std::string &line : snip->lines) {
		line.insert(line.begin(), '\t');
	}

	return snip;
}

QbeBackend::Snippet *QbeBackend::VisitExprLoadReceiver(ExprLoadReceiver *node) {
	Snippet *snip = m_alloc.New<Snippet>();
	snip->result = m_alloc.New<VLocal>();
	snip->result->name = "this"; // See VisitFn, it's hardcoded in the function arguments
	return snip;
}

QbeBackend::Snippet *QbeBackend::VisitExprAllocateInstanceMemory(ExprAllocateInstanceMemory *node) {
	Snippet *snip = m_alloc.New<Snippet>();
	std::string name = node->target->info->name;
	snip->result = AddTemporary("new_instance_" + name);
	VLocal *classValue = AddTemporary("new_instance_" + name);
	snip->Add("%{} =l loadl $class_var_{}", classValue->name, name);
	snip->Add("%{} =l call $wren_alloc_obj(l %{})", snip->result->name, classValue->name);
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
	local->name = MangleUniqueName(debugName, true);
	return local;
}

std::string QbeBackend::GetStringPtr(const std::string &value) {
	auto iter = m_strings.find(value);
	if (iter != m_strings.end())
		return iter->second;

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

std::string QbeBackend::GetStringObjPtr(const std::string &value) {
	auto iter = m_stringObjs.find(value);
	if (iter != m_stringObjs.end())
		return iter->second;

	std::string rawSymbol = GetStringPtr(value);
	std::string symbol = "$obj_" + rawSymbol.substr(1);
	m_stringObjs[value] = symbol;
	return symbol;
}

std::string QbeBackend::EscapeString(std::string value) {
	for (int i = 0; i < value.size(); i++) {
		char c = value.at(i);
		if (c != '"' && c != '\\')
			continue;

		// Insert an escape string
		value.insert(value.begin() + i, '\\');

		// Skip the character, otherwise we'll keep escaping it in an infinite loop
		i++;
	}
	return value;
}

QbeBackend::VLocal *QbeBackend::LookupVariable(LocalVariable *decl) {
	auto iter = m_locals.find(decl);
	if (iter != m_locals.end())
		return iter->second.get();

	m_locals[decl] = std::make_unique<VLocal>();
	VLocal *local = m_locals.at(decl).get();
	local->name = "lv_" + MangleUniqueName(decl->name, false); // lv for local variable
	return local;
}

std::string QbeBackend::MangleGlobalName(IRGlobalDecl *var) {
	// TODO add module name etc
	return "$global_var_" + MangleRawName(var->name, false);
}

std::string QbeBackend::MangleUniqueName(const std::string &name, bool excludeIdentical) {
	if (!excludeIdentical) {
		auto iter = m_uniqueNames.find(name);
		if (iter != m_uniqueNames.end())
			return iter->second;
	}

	std::string base = MangleRawName(name, true);

	// If required, put on a numerical suffix
	std::string mangled = base;
	int i = 1;
	while (m_uniqueNamesInv.contains(mangled)) {
		mangled = base + "_" + std::to_string(i++);
	}

	m_uniqueNames[name] = mangled;
	m_uniqueNamesInv.insert(mangled);
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
QbeBackend::Snippet *QbeBackend::VisitExprLogicalNot(ExprLogicalNot *node) { return HandleUnimplemented(node); }
