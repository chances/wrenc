//
// Created by znix on 28/12/22.
//

#include "IRPrinter.h"
#include "ClassInfo.h"
#include "CompContext.h"
#include "Scope.h"

#include <memory>
#include <set>
#include <sstream>

#include <fmt/format.h>

// Nodes to display inline in the IR dump
const std::set<std::string> INLINE_NODES = {
    typeid(StmtReturn).name(),
    typeid(StmtJump).name(),
    typeid(ExprLoad).name(),
};
const bool PRINT_ADDR = false;

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

void IRPrinter::VisitReadOnly(IRNode *node) {
	if (!node)
		return;

	// Default to our typename
	std::string name = typeid(*node).name();
	std::string header = name;
	if (PRINT_ADDR)
		header += fmt::format(" {}", (void *)node);
	StartTag(header, INLINE_NODES.contains(name));
	IRVisitor::VisitReadOnly(node);
	EndTag();
}

void IRPrinter::Process(IRNode *root) { VisitReadOnly(root); }

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
	if (node->super) {
		m_tagStack.back().header += " SUPER";
	}
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
	if (node->jumpOnFalse)
		header += " INVERTED";
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

void IRPrinter::VisitExprSystemVar(ExprSystemVar *node) {
	m_tagStack.back().header += " " + node->name;
	IRVisitor::VisitExprSystemVar(node);
}

void IRPrinter::VisitExprGetClassVar(ExprGetClassVar *node) {
	m_tagStack.back().header += " " + node->cls->info->name;
	IRVisitor::VisitExprGetClassVar(node);
}

void IRPrinter::VisitFn(IRFn *node) {
	m_tagStack.back().header += " " + node->debugName;

	// Add a tag with the arguments
	std::string args = "args";
	for (LocalVariable *arg : node->parameters) {
		args += " " + arg->name;
	}
	StartTag(args, true);
	EndTag();

	IRVisitor::VisitFn(node);
}

void IRPrinter::VisitStmtFieldAssign(StmtFieldAssign *node) {
	m_tagStack.back().header += " " + node->var->Name();
	IRVisitor::VisitStmtFieldAssign(node);
}
void IRPrinter::VisitExprFieldLoad(ExprFieldLoad *node) {
	m_tagStack.back().header += " " + node->var->Name();
	IRVisitor::VisitExprFieldLoad(node);
}

void IRPrinter::VisitExprAllocateInstanceMemory(ExprAllocateInstanceMemory *node) {
	m_tagStack.back().header += " " + node->target->info->name;
	IRVisitor::VisitExprAllocateInstanceMemory(node);
}

void IRPrinter::VisitExprClosure(ExprClosure *node) {
	m_tagStack.back().header += " " + node->func->debugName;
	IRVisitor::VisitExprClosure(node);
}

void IRPrinter::VisitStmtRelocateUpvalues(StmtRelocateUpvalues *node) {
	for (LocalVariable *var : node->variables)
		m_tagStack.back().header += " " + var->name;
	IRVisitor::VisitStmtRelocateUpvalues(node);
}
