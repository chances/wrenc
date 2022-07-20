//
// Created by znix on 10/07/2022.
//

#include "IRNode.h"

#include "ArenaAllocator.h"
#include "ClassInfo.h"
#include "CompContext.h"
#include "Scope.h"

#include <fmt/format.h>
#include <set>
#include <sstream>

// Nodes to display inline in the IR dump
const std::set<std::string> INLINE_NODES = {
    typeid(StmtReturn).name(),
    typeid(StmtJump).name(),
    typeid(ExprLoad).name(),
    typeid(ExprLogicalNot).name(),
};
const bool PRINT_ADDR = false;

// Ugly hack to get the allocator out of a compiler
ArenaAllocator *getCompilerAlloc(Compiler *compiler);

IRClass::~IRClass() = default;
VarDecl::~VarDecl() = default;
IRNode::~IRNode() = default;

std::string IRGlobalDecl::Name() const { return name; }
VarDecl::ScopeType IRGlobalDecl::Scope() const { return SCOPE_MODULE; }

void StmtBlock::Add(IRStmt *stmt) {
	if (stmt)
		statements.push_back(stmt);
}
void StmtBlock::Add(Compiler *forAlloc, IRExpr *expr) {
	if (!expr)
		return;

	ArenaAllocator *alloc = getCompilerAlloc(forAlloc);
	Add(alloc->New<StmtEvalAndIgnore>(expr));
}

ExprConst::ExprConst() {}
ExprConst::ExprConst(CcValue value) : value(value) {}

std::string StmtUpvalueImport::Name() const { return parent->Name() + "/UPVALUE"; }
VarDecl::ScopeType StmtUpvalueImport::Scope() const { return SCOPE_UPVALUE; }

ExprSystemVar::ExprSystemVar(std::string name) : name(name) {
	if (!SYSTEM_VAR_NAMES.contains(name)) {
		fmt::print(stderr, "Illegal system variable name '{}'\n", name);
		abort();
	}
}
const std::unordered_set<std::string> ExprSystemVar::SYSTEM_VAR_NAMES = {
    "Bool", "Class", "Fiber", "Fn", "List", "Map", "Null", "Num", "Object", "Range", "Sequence", "String", "System",
};

IRVisitor::~IRVisitor() {}

// Top-level-node accept functions
void IRFn::Accept(IRVisitor *visitor) { visitor->VisitFn(this); }
void IRClass::Accept(IRVisitor *visitor) { visitor->VisitClass(this); }
void IRGlobalDecl::Accept(IRVisitor *visitor) { visitor->VisitGlobalDecl(this); }
void IRImport::Accept(IRVisitor *visitor) { visitor->VisitImport(this); }

// Statements
void StmtAssign::Accept(IRVisitor *visitor) { visitor->VisitStmtAssign(this); }
void StmtFieldAssign::Accept(IRVisitor *visitor) { visitor->VisitStmtFieldAssign(this); }
void StmtUpvalueImport::Accept(IRVisitor *visitor) { visitor->VisitStmtUpvalue(this); }
void StmtEvalAndIgnore::Accept(IRVisitor *visitor) { visitor->VisitStmtEvalAndIgnore(this); }
void StmtBlock::Accept(IRVisitor *visitor) { visitor->VisitBlock(this); } // Use Block not StmtBlock since it's special
void StmtLabel::Accept(IRVisitor *visitor) { visitor->VisitStmtLabel(this); }
void StmtJump::Accept(IRVisitor *visitor) { visitor->VisitStmtJump(this); }
void StmtReturn::Accept(IRVisitor *visitor) { visitor->VisitStmtReturn(this); }
void StmtLoadModule::Accept(IRVisitor *visitor) { visitor->VisitStmtLoadModule(this); }

// Expressions
void ExprConst::Accept(IRVisitor *visitor) { visitor->VisitExprConst(this); }
void ExprLoad::Accept(IRVisitor *visitor) { visitor->VisitExprLoad(this); }
void ExprFieldLoad::Accept(IRVisitor *visitor) { visitor->VisitExprFieldLoad(this); }
void ExprFuncCall::Accept(IRVisitor *visitor) { visitor->VisitExprFuncCall(this); }
void ExprClosure::Accept(IRVisitor *visitor) { visitor->VisitExprClosure(this); }
void ExprLoadReceiver::Accept(IRVisitor *visitor) { visitor->VisitExprLoadReceiver(this); }
void ExprRunStatements::Accept(IRVisitor *visitor) { visitor->VisitExprRunStatements(this); }
void ExprLogicalNot::Accept(IRVisitor *visitor) { visitor->VisitExprLogicalNot(this); }
void ExprAllocateInstanceMemory::Accept(IRVisitor *visitor) { visitor->VisitExprAllocateInstanceMemory(this); }
void ExprSystemVar::Accept(IRVisitor *visitor) { visitor->VisitExprSystemVar(this); }

// IRVisitor

void IRVisitor::Visit(IRNode *node) {
	if (node)
		node->Accept(this);
}
void IRVisitor::VisitVar(VarDecl *var) {
	if (var)
		var->Accept(this);
}

void IRVisitor::VisitFn(IRFn *node) {
	for (LocalVariable *var : node->locals)
		var->Accept(this);

	for (const auto &entry : node->upvalues) {
		VisitVar(entry.first);
		// Ignore entry.second because upvalue imports are scattered around the tree
	}

	for (StmtUpvalueImport *import : node->unInsertedImports)
		Visit(import);

	Visit(node->body);
}
void IRVisitor::VisitClass(IRClass *node) {}
void IRVisitor::VisitGlobalDecl(IRGlobalDecl *node) {}
void IRVisitor::VisitImport(IRImport *node) {}
void IRVisitor::VisitStmtAssign(StmtAssign *node) {
	VisitVar(node->var);
	Visit(node->expr);
}
void IRVisitor::VisitStmtFieldAssign(StmtFieldAssign *node) {
	// node->var isn't a VarDecl
	Visit(node->object);
	Visit(node->value);
}
void IRVisitor::VisitStmtUpvalue(StmtUpvalueImport *node) { VisitVar(node->parent); }
void IRVisitor::VisitStmtEvalAndIgnore(StmtEvalAndIgnore *node) { Visit(node->expr); }
void IRVisitor::VisitBlock(StmtBlock *node) {
	for (IRStmt *stmt : node->statements)
		Visit(stmt);
}
void IRVisitor::VisitStmtLabel(StmtLabel *node) {}
void IRVisitor::VisitStmtJump(StmtJump *node) { Visit(node->condition); }
void IRVisitor::VisitStmtReturn(StmtReturn *node) { Visit(node->value); }
void IRVisitor::VisitStmtLoadModule(StmtLoadModule *node) {
	for (const StmtLoadModule::VarImport &import : node->variables)
		VisitVar(import.bindTo);
}
void IRVisitor::VisitExprConst(ExprConst *node) {}
void IRVisitor::VisitExprLoad(ExprLoad *node) { VisitVar(node->var); }
void IRVisitor::VisitExprFieldLoad(ExprFieldLoad *node) { Visit(node->object); }
void IRVisitor::VisitExprFuncCall(ExprFuncCall *node) {
	Visit(node->receiver);
	for (IRExpr *expr : node->args)
		Visit(expr);
}
void IRVisitor::VisitExprClosure(ExprClosure *node) {
	// Closures are only generated for anonymous functions, which are only accessible through these nodes, so
	// it's fine to visit them.
	Visit(node->func);
}
void IRVisitor::VisitExprLoadReceiver(ExprLoadReceiver *node) {}
void IRVisitor::VisitExprRunStatements(ExprRunStatements *node) {
	VisitVar(node->temporary);
	Visit(node->statement);
}
void IRVisitor::VisitExprLogicalNot(ExprLogicalNot *node) { Visit(node->input); }
void IRVisitor::VisitExprAllocateInstanceMemory(ExprAllocateInstanceMemory *node) {
	// Our IRClass nodes are already in the tree, don't visit them
}
void IRVisitor::VisitExprSystemVar(ExprSystemVar *node) {}

void IRVisitor::VisitLocalVariable(LocalVariable *var) {}

// Node IR printing

// IR Printer
IRPrinter::IRPrinter() { m_stream = std::make_unique<std::stringstream>(); }
IRPrinter::~IRPrinter() {}

void IRPrinter::FullTag(const std::string &str) {
	StartTag(str, true);
	EndTag();
}

void IRPrinter::StartTag(const std::string &str, bool isInline) {
	m_tagStack.emplace_back(Tag{
	    .header = str,
	    .isInline = isInline,
	});
}
void IRPrinter::EndTag() {
	Tag &tag = m_tagStack.back();

	std::stringstream result;
	result << "[" << tag.header;

	std::string indent = "  ";

	for (std::string part : tag.components) {
		if (tag.isInline)
			result << " ";
		else
			result << "\n" << indent;

		// Add a level of indentation
		for (int i = part.length() - 1; i >= 0; i--) {
			if (part.at(i) != '\n')
				continue;
			part.insert(i + 1, indent);
		}

		result << part;
	}

	if (tag.components.empty() || tag.isInline)
		result << "]";
	else
		result << "\n]";

	// Remove this tag
	m_tagStack.pop_back();

	// If there's another tag below us, add onto that. Otherwise, add to the main output
	if (!m_tagStack.empty())
		m_tagStack.back().components.emplace_back(result.str());
	else
		*m_stream << result.str() << "\n";
}

std::unique_ptr<std::stringstream> IRPrinter::Extract() { return std::move(m_stream); }

void IRPrinter::Visit(IRNode *node) {
	if (!node)
		return;

	// Default to our typename
	std::string name = typeid(*node).name();
	std::string header = name;
	if (PRINT_ADDR)
		header += fmt::format(" {}", (void *)node);
	StartTag(header, INLINE_NODES.contains(name));
	IRVisitor::Visit(node);
	EndTag();
}

void IRPrinter::Process(IRNode *root) { Visit(root); }

void IRPrinter::VisitVar(VarDecl *var) { FullTag(std::string(typeid(*var).name()) + ":" + var->Name()); }

void IRPrinter::VisitExprConst(ExprConst *node) {
	std::string value = "INVALID_TYPE";
	switch (node->value.type) {
	case CcValue::UNDEFINED:
		value = "undefined";
		break;
	case CcValue::NULL_TYPE:
		value = "null";
		break;
	case CcValue::STRING:
		value = "'" + node->value.s + "'";
		break;
	case CcValue::BOOL:
		value = "bool " + std::to_string(node->value.b);
		break;
	case CcValue::INT:
		value = "int " + std::to_string(node->value.i);
		break;
	case CcValue::NUM:
		value = "num " + std::to_string(node->value.n);
		break;
	}
	m_tagStack.back().header += " " + value;
}

void IRPrinter::VisitExprFuncCall(ExprFuncCall *node) {
	m_tagStack.back().header += " " + node->signature->ToString();
	IRVisitor::VisitExprFuncCall(node);
}
void IRPrinter::VisitStmtLabel(StmtLabel *node) {
	m_tagStack.back().header += " id:" + GetLabelId(node);
	IRVisitor::VisitStmtLabel(node);
}
void IRPrinter::VisitStmtJump(StmtJump *node) {
	std::string &header = m_tagStack.back().header;
	if (node->looping)
		header += " LOOPING";
	header += " target:" + GetLabelId(node->target);
	IRVisitor::VisitStmtJump(node);
}

std::string IRPrinter::GetLabelId(StmtLabel *label) {
	if (label == nullptr)
		return "null";

	int id = 0;
	auto iter = m_labelIds.find(label);
	if (iter != m_labelIds.end()) {
		id = iter->second;
	} else {
		id = m_labelIds.size() + 1;
		m_labelIds[label] = id;
	}

	std::string str;
	if (!label->debugName.empty())
		str += label->debugName + " ";
	str += std::to_string(id);
	return str;
}
