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

static const std::string SYM_FIELD_OFFSET = "class_field_offset_";
static const std::string SYM_CLOSURE_DATA_TBL = "closure_data_";
static const std::string SYM_CLOSURE_DESC_OBJ = "closure_obj_";

QbeBackend::QbeBackend() = default;
QbeBackend::~QbeBackend() = default;

std::string QbeBackend::Generate(Module *module) {
	std::string moduleName = MangleUniqueName(module->Name().value_or("unknown"), false);

	// Make the upvalue pack for each function that needs them
	for (IRFn *func : module->GetClosures()) {
		std::unique_ptr<UpvaluePackDef> pack = std::make_unique<UpvaluePackDef>();

		for (const auto &entry : func->upvalues) {
			// Assign an increasing series of IDs for each variable in an arbitrary order
			pack->variables.push_back(entry.second);
			pack->variableIds[entry.second] = pack->variables.size() - 1; // -1 to get the index of the last entry
		}

		// If this pack doesn't have any upvalues, then don't register it's pack. This means it's signature
		// won't include the upvalue pack as the first argument, which should slightly improve performance.
		if (pack->variableIds.empty())
			continue;

		m_functionUpvaluePacks[func] = std::move(pack);
	}

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

		// Write the fields
		for (const std::unique_ptr<FieldVariable> &var : cls->info->fields.fields) {
			Print("w {} {},", (int)Cmd::ADD_FIELD, 0);
			Print("{} {},", PTR_TYPE, GetStringPtr(var->Name()));
		}

		Print("w {} 0,", (int)Cmd::END, CD::FLAG_NONE); // Signal the end of the class description
		Print("}}");
	}

	// Add a table describing all the closures, and pointers to data objects for them
	for (IRFn *fn : module->GetClosures()) {
		std::string funcName = MangleUniqueName(fn->debugName, false);
		Print("section \".bss\" data ${}{} = {{ {} 0 }}", SYM_CLOSURE_DESC_OBJ, GetClosureName(fn), PTR_TYPE);
		Print("section \".rodata\" data ${}{} = {{ ", SYM_CLOSURE_DATA_TBL, GetClosureName(fn));
		Print("\t{} ${},", PTR_TYPE, funcName);
		Print("\t{} {},", PTR_TYPE, GetStringPtr(fn->debugName));
		Print("\tw {0}, w {1}, # arity={0} num_upvalues={1}", fn->arity, fn->upvalues.size());

		// Keep track of how many words are written so we can keep the section qword-aligned
		int words = 0;

		// Write the upvalue indices
		auto upvaluePackIter = m_functionUpvaluePacks.find(fn);
		if (upvaluePackIter != m_functionUpvaluePacks.end()) {
			UpvaluePackDef *pack = upvaluePackIter->second.get();
			for (UpvalueVariable *upvalue : pack->variables) {
				Print("\tw {}, # Parent stack position of variable '{}'", pack->valuesOnParentStack.at(upvalue),
				      upvalue->parent->Name());
				words += 1;
			}
		}

		// Pad to achieve 64-bit alignment if necessary
		if (words % 2 == 1) {
			Print("\tw -1, # Alignment padding");
		}

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

	// Add fields to store the offset of the member fields in each class. This is required since we
	// can currently have classes extending from classes in another file, and they don't know how
	// many fields said classes have. Thus this field will get loaded at startup as a byte offset and
	// we have to add it to the object pointer to get the fields area.
	for (IRClass *cls : module->GetClasses()) {
		// Make it word-sized, that'll be big enough and hopefully help with getting more into the cache
		Print("section \".bss\" data ${}{} = {{ w {} }}", SYM_FIELD_OFFSET, cls->info->name, 0);
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

	return m_output.str();
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

		// Read the field offset
		std::string offsetVarName = fmt::format("tmp_field_offset_{}", cls->info->name);
		Print("%{} =l call $wren_class_get_field_offset(l %{})", offsetVarName, varName);
		Print("storel %{}, ${}{}", offsetVarName, SYM_FIELD_OFFSET, cls->info->name);
	}

	Print("# Register upvalues");
	for (IRFn *fn : module->GetClosures()) {
		// For each upvalue, tell the runtime about it and save the description object it gives us. This object
		// is then used to closure objects wrapping this function.
		std::string name = GetClosureName(fn);
		std::string dataName = fmt::format("tmp_upvalue_{}", name);
		Print("%{} ={} call $wren_register_closure({} ${}{})", dataName, PTR_TYPE, PTR_TYPE, SYM_CLOSURE_DATA_TBL,
		      name);
		Print("store{} %{}, ${}{}", PTR_TYPE, dataName, SYM_CLOSURE_DESC_OBJ, name);
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

	if (m_functionUpvaluePacks.contains(node)) {
		m_currentFnUpvaluePack = m_functionUpvaluePacks.at(node).get();
	}

	std::string args;
	// Check if this is a method or not. This is non-null for static methods, but they also have receivers.
	if (node->enclosingClass) {
		// Add the receiver
		args += "l %this, ";
	}
	if (m_currentFnUpvaluePack) {
		// Upvalues take the pointer to a pack of variables as their first argument, but for convenience in the
		// calling code we widen it into the size of a value. This might need some changes for 32-bit support (if
		// QBE ever gets support for that).
		args += std::string("l %upvalue_pack, ");
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

	// If a function supplies upvalues to a closure contained within it, that closure needs to have a pointer to the
	// values in the parent function. Once the parent function returns these values live on the heap, but before that
	// they live on the stack. This code allocates stack space for all the variables referenced by closures.
	// A function might have many variables and many closures, and those closures can use any set of those variables.
	// Thus we can't have an array of values per closure. But passing in a bunch of different value pointers when we
	// create a closure would be a big pain, so instead we create one array of values for all the variables used by
	// closures. Each closure's data table encodes the offsets of the variables it cares about in this table, and we
	// can pass it as a single pointer argument to the closure creation function.
	int variablesUsedAsUpvalues = 0;
	for (LocalVariable *var : node->locals) {
		if (var->upvalues.empty())
			continue;

		// The position of this variable in the stack is equal to the current number of upvalues, since they're placed
		// in the order they appear here.
		int stackIndex = variablesUsedAsUpvalues;

		// Put it in the local stack variables array, so we know to access it on the stack and what it's index is
		if (m_stackVariables.contains(var)) {
			fmt::print(stderr, "Cannot add variable '{}' to the stack again in function '{}'\n", var->Name(),
			           node->debugName);
		}
		m_stackVariables[var] = stackIndex;

		// Mark each variable's position on the stack. This gets baked into the closure data block so that when
		// we create a closure, we just pass our variable stack in and it pulls out the correct values.
		for (UpvalueVariable *up : var->upvalues) {
			auto iter = m_functionUpvaluePacks.find(up->containingFunction);
			if (iter == m_functionUpvaluePacks.end()) {
				fmt::print(stderr, "Upvalue '{}' references function '{}' which doesn't have an upvalue pack\n",
				           up->parent->Name(), up->containingFunction->debugName);
				abort();
			}

			UpvaluePackDef *pack = iter->second.get();

			if (pack->valuesOnParentStack.contains(up)) {
				fmt::print(
				    stderr,
				    "Upvalue '{}' in function '{}' has already been allocated a source stack position when building "
				    "function '{}'\n",
				    up->parent->Name(), up->containingFunction->debugName, node->debugName);
				abort();
			}
			pack->valuesOnParentStack[up] = stackIndex;
		}

		variablesUsedAsUpvalues++;
	}
	if (variablesUsedAsUpvalues != 0) {
		Print("%stack_locals ={} alloc8 {}", PTR_TYPE, variablesUsedAsUpvalues * sizeof(Value));
	}

	// If this is a method, load the field offset. We're probably going to access our fields (if not, QBE should
	// be able to remove this load?) so it's good to only have to load their offset once
	// For more information about these offsets, see the comment on the code generating this symbol for more information
	if (node->enclosingClass) {
		Print("%this_ptr =l and %this, {}", CONTENT_MASK); // This is equivalent to get_object_value
		Print("%this_field_start_offset =w loadw ${}{}", SYM_FIELD_OFFSET, node->enclosingClass->info->name);
		Print("%this_field_start =l extuw %this_field_start_offset");   // Unsigned extend word->long
		Print("%this_field_start =l add %this_field_start, %this_ptr"); // Add this to get the field area pointer
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

	m_currentFnUpvaluePack = nullptr;
	m_stackVariables.clear();
	Print("}}");
}

QbeBackend::Snippet *QbeBackend::VisitStmtAssign(StmtAssign *node) {
	Snippet *snip = m_alloc.New<Snippet>();

	VLocal *input = snip->Add(VisitExpr(node->expr));

	auto stackLocalIter = m_stackVariables.find(node->var);
	LocalVariable *local = dynamic_cast<LocalVariable *>(node->var);
	IRGlobalDecl *global = dynamic_cast<IRGlobalDecl *>(node->var);
	if (stackLocalIter != m_stackVariables.end()) {
		VLocal *localDerefTemp = AddTemporary("assign_stack_local_ptr_" + node->var->Name());
		snip->Add("%{} ={} add %stack_locals, {}", localDerefTemp->name, PTR_TYPE,
		          stackLocalIter->second * sizeof(Value));
		snip->Add("storel %{}, %{}", input->name, localDerefTemp->name);
	} else if (local) {
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
	auto stackLocalIter = m_stackVariables.find(node->var);
	LocalVariable *local = dynamic_cast<LocalVariable *>(node->var);
	IRGlobalDecl *global = dynamic_cast<IRGlobalDecl *>(node->var);
	UpvalueVariable *upvalue = dynamic_cast<UpvalueVariable *>(node->var);
	Snippet *snip = m_alloc.New<Snippet>();
	snip->result = AddTemporary("var_load_" + node->var->Name());
	if (stackLocalIter != m_stackVariables.end()) {
		VLocal *localDerefTemp = AddTemporary("stack_local_ptr_" + node->var->Name());
		snip->Add("%{} ={} add %stack_locals, {}", localDerefTemp->name, PTR_TYPE,
		          stackLocalIter->second * sizeof(Value));
		snip->Add("%{} =l loadl %{}", snip->result->name, localDerefTemp->name);
	} else if (local) {
		snip->Add("%{} ={} copy %{}", snip->result->name, PTR_TYPE, LookupVariable(local)->name);
	} else if (global) {
		snip->Add("%{} ={} load{} {}", snip->result->name, PTR_TYPE, PTR_TYPE, MangleGlobalName(global));
	} else if (upvalue) {
		if (!m_currentFnUpvaluePack) {
			fmt::print(stderr, "Found UpvalueVariable without an upvalue pack!\n");
			abort();
		}

		auto upvalueIter = m_currentFnUpvaluePack->variableIds.find(upvalue);
		if (upvalueIter == m_currentFnUpvaluePack->variableIds.end()) {
			fmt::print(stderr, "Could not find upvalue in current pack for variable {}\n", upvalue->parent->Name());
			abort();
		}
		int offset = upvalueIter->second * sizeof(Value);

		// Get a pointer pointing to the position in the upvalue pack where this variable is
		VLocal *valuePtr = AddTemporary("upvalue_pack_sub_ptr");
		snip->Add("%{} ={} add %upvalue_pack, {}", valuePtr->name, PTR_TYPE, offset);

		// The upvalue pack stores pointers, so at this point our variable is a Value** and we have to dereference
		// it twice. In the future we should store never-modified variables directly, so this would be Value*.
		// This chases the pointer in-place, that is in the same variable, to avoid the bother of declaring another one.
		snip->Add("%{} ={} load{} %{}", valuePtr->name, PTR_TYPE, PTR_TYPE, valuePtr->name);

		// Load the value itself
		snip->Add("%{} =l loadl %{}", snip->result->name, valuePtr->name);
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

QbeBackend::Snippet *QbeBackend::VisitStmtLabel(StmtLabel *node) {
	Snippet *snip = m_alloc.New<Snippet>();
	snip->Add("@{}", GetLabelName(node));
	return snip;
}

QbeBackend::Snippet *QbeBackend::VisitStmtJump(StmtJump *node) {
	Snippet *snip = m_alloc.New<Snippet>();

	// Unconditional jumps are easy :)
	if (!node->condition) {
		snip->Add("jmp @{}", GetLabelName(node->target));
		return snip;
	}

	// Control doesn't automatically fall through to the next block, so we have to add a label for it
	// There's also an intermediate step where we know something isn't null, but don't know whether or
	// not it's false.
	std::string fallthroughLabel = fmt::format("fallthrough_{:016x}", (uint64_t)node);
	std::string checkFalseLabel = fmt::format("if_check_false_{:016x}", (uint64_t)node);

	// Switch around the jump targets depending on whether or not the jump is inverted.
	std::string falseTarget = fallthroughLabel;
	std::string trueTarget = GetLabelName(node->target);
	if (node->jumpOnFalse) {
		std::swap(falseTarget, trueTarget);
	}

	// First evaluate the expression
	VLocal *expr = snip->Add(VisitExpr(node->condition));

	// Check if the returned value is null. Unfortunately Wren has two 'falsey' values - null and false.
	// First check null, because it's quicker - check if it matches a constant, and if so the condition is
	// false and jump to the fallthrough label.
	VLocal *isNull = AddTemporary("if_null");
	snip->Add("%{} =w ceql %{}, {}", isNull->name, expr->name, NULL_VAL);
	snip->Add("jnz %{}, @{}, @{}", isNull->name, falseTarget, checkFalseLabel);

	// Add the false-ness check. One big disadvantage of our current system for storing booleans is that
	// we have to load a global variable storing the false value's pointer. Statically allocating the
	// false value so the linker can find the value doesn't work for shared libraries, otherwise it'd be
	// a very clean solution.
	snip->Add("@{}", checkFalseLabel);
	VLocal *isFalse = AddTemporary("if_false");
	VLocal *falsePtr = AddTemporary("if_false_ptr");
	snip->Add("%{} =l loadl $wren_sys_bool_false", falsePtr->name);
	snip->Add("%{} =w ceql %{}, %{}", isFalse->name, falsePtr->name, expr->name);
	snip->Add("jnz %{}, @{}, @{}", isFalse->name, falseTarget, trueTarget);

	snip->Add("@{}", fallthroughLabel);
	return snip;
}

QbeBackend::Snippet *QbeBackend::VisitStmtFieldAssign(StmtFieldAssign *node) {
	// The byte offset of the target field, relative to the first field
	// Since Value is eight bytes wide (even on a hypothetical 32-bit port), multiply by that.
	int fieldPos = node->var->Id() * 8;

	Snippet *snip = m_alloc.New<Snippet>();

	VLocal *value = snip->Add(VisitExpr(node->value));

	// Find the address of the field to write to
	VLocal *targetFieldOffset = AddTemporary("field_ptr_" + node->var->Name());
	snip->Add("%{} =l add %this_field_start, {}", targetFieldOffset->name, fieldPos);

	// Write out the field
	snip->Add("storel %{}, %{}", value->name, targetFieldOffset->name);

	return snip;
}

QbeBackend::Snippet *QbeBackend::VisitExprFieldLoad(ExprFieldLoad *node) {
	// See VisitStmtFieldAssign for comments explaining this
	int fieldPos = node->var->Id() * 8;

	Snippet *snip = m_alloc.New<Snippet>();
	VLocal *targetFieldOffset = AddTemporary("field_ptr_" + node->var->Name());
	snip->result = AddTemporary("field_load_" + node->var->Name());
	snip->Add("%{} =l add %this_field_start, {}", targetFieldOffset->name, fieldPos);
	snip->Add("%{} =l loadl %{}", snip->result->name, targetFieldOffset->name);

	return snip;
}

QbeBackend::Snippet *QbeBackend::VisitExprClosure(ExprClosure *node) {
	Snippet *snip = m_alloc.New<Snippet>();

	// Get a description object storing the properties of this closure
	std::string name = GetClosureName(node->func);
	VLocal *descObj = AddTemporary("closure_desc_" + name);
	Print("%{} ={} load{} ${}{}", descObj->name, PTR_TYPE, PTR_TYPE, SYM_CLOSURE_DESC_OBJ, name);

	// And use the description object to request a new closure over it
	// Pass in our stack, as the closure will bind to values on it
	const char *stackLocals = node->func->upvalues.empty() ? "0" : "%stack_locals";
	snip->result = AddTemporary("closure_result_" + node->func->debugName);
	snip->Add("%{} =l call $wren_create_closure({} %{}, {} {})", snip->result->name, PTR_TYPE, descObj->name, PTR_TYPE,
	          stackLocals);

	// TODO upvalue handling

	return snip;
}

// Utility functions //

QbeBackend::Snippet *QbeBackend::HandleUnimplemented(IRNode *node) {
	fmt::print(stderr, "QbeBackend: Unsupported node {}\n", typeid(*node).name());
	abort();
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

std::string QbeBackend::GetClosureName(IRFn *func) { return MangleUniqueName(func->debugName, false); }

std::string QbeBackend::GetLabelName(StmtLabel *label) {
	return fmt::format("lbl_{:016x}_{}", (uint64_t)label, MangleRawName(label->debugName, true));
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
QbeBackend::Snippet *QbeBackend::VisitBlock(StmtBlock *node) { return HandleUnimplemented(node); }
QbeBackend::Snippet *QbeBackend::VisitStmtLoadModule(StmtLoadModule *node) { return HandleUnimplemented(node); }
