//
// Created by znix on 28/12/22.
//

#include "IRPrinter.h"
#include "ClassInfo.h"
#include "CompContext.h"
#include "HashUtil.h"
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
	    .indent = "  ",
	    .isInline = isInline,
	});
}
void IRPrinter::EndTag() {
	Tag &tag = m_tagStack.back();

	std::stringstream result;
	result << "[" << tag.header;

	for (std::string part : tag.components) {
		if (tag.isInline)
			result << " ";
		else
			result << "\n" << tag.indent;

		// Add a level of indentation while writing the tag to the result stream
		for (char c : part) {
			result << c;
			if (c == '\n') {
				result << tag.indent;
			}
		}
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

void IRPrinter::Process(IRNode *root) {
	// If this is a function that's in basic-block form, note that down and we'll
	// use it to evenly distribute the label colours.
	m_basicBlockCount = -1;
	IRFn *fn = dynamic_cast<IRFn *>(root);
	if (fn != nullptr && !fn->body->statements.empty()) {
		StmtBlock *firstChild = dynamic_cast<StmtBlock *>(fn->body->statements.at(0));
		if (firstChild != nullptr && firstChild->isBasicBlock) {
			m_basicBlockCount = fn->body->statements.size();
		}
	}

	VisitReadOnly(root);
}

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
	if (node->superCaller) {
		m_tagStack.back().header += " SUPER:" + node->superCaller->debugName;
	}
	m_tagStack.back().header += " " + node->signature->ToString();
	IRVisitor::VisitExprFuncCall(node);
}
void IRPrinter::VisitStmtLabel(StmtLabel *node) {
	// Don't colourise the label name - this way, the colours always indicate a jump
	m_tagStack.back().header += " id:" + GetLabelId(node, false);
	IRVisitor::VisitStmtLabel(node);
}
void IRPrinter::VisitStmtJump(StmtJump *node) {
	std::string &header = m_tagStack.back().header;
	if (node->looping)
		header += " LOOPING";
	if (node->jumpOnFalse)
		header += " INVERTED";
	header += " target:" + GetLabelId(node->target, true);
	IRVisitor::VisitStmtJump(node);
}

std::string IRPrinter::GetLabelId(StmtLabel *label, bool colourise) {
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

	// If we're in basic-block form, colour-code the name to match the basic block it's contained in.
	// This just makes it easier to see what's jumping where at a glance.
	if (label->basicBlock && colourise) {
		str = Colourise(label->basicBlock, str);
	}

	return str;
}

std::string IRPrinter::GetBeginUpvaluesId(StmtBeginUpvalues *upvalue) {
	if (upvalue == nullptr)
		return "null";

	int id = 0;
	auto iter = m_beginUpvaluesIds.find(upvalue);
	if (iter != m_beginUpvaluesIds.end()) {
		id = iter->second;
	} else {
		id = m_beginUpvaluesIds.size() + 1;
		m_beginUpvaluesIds[upvalue] = id;
	}

	return std::to_string(id);
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

	// Add a tag for the root upvalues
	if (node->rootBeginUpvalues) {
		m_tagStack.back().header += " ROOT_UV:" + GetBeginUpvaluesId(node->rootBeginUpvalues);
	}

	// Add a tag with the arguments
	std::string args = "args";
	for (SSAVariable *arg : node->parameters) {
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

void IRPrinter::VisitStmtBeginUpvalues(StmtBeginUpvalues *node) {
	m_tagStack.back().header += " ID:" + GetBeginUpvaluesId(node);

	for (LocalVariable *var : node->variables)
		m_tagStack.back().header += " " + var->name;

	IRVisitor::VisitStmtBeginUpvalues(node);
}

void IRPrinter::VisitStmtRelocateUpvalues(StmtRelocateUpvalues *node) {
	for (StmtBeginUpvalues *upvalues : node->upvalueSets) {
		m_tagStack.back().header += " " + GetBeginUpvaluesId(upvalues);
	}
	IRVisitor::VisitStmtRelocateUpvalues(node);
}

void IRPrinter::VisitBlock(StmtBlock *node) {
	if (node->isBasicBlock) {
		Tag &tag = m_tagStack.back();
		tag.header += " BASIC";
		tag.indent = Colourise(node, "| ");
	}

	IRVisitor::VisitBlock(node);
}

std::string IRPrinter::Colourise(IRNode *ptr, const std::string &input) {
	// If we're in basic block mode, colourise basic blocks specially
	StmtBlock *block = dynamic_cast<StmtBlock *>(ptr);
	if (m_basicBlockCount != -1 && block) {
		// Find or assign an ID for this basic block.
		int id;
		auto iter = m_basicBlockIds.find(block);
		if (iter != m_basicBlockIds.end()) {
			id = iter->second;
		} else {
			id = m_basicBlockIds.size();
			m_basicBlockIds[block] = id;
		}

		// Use that to build a colour, based on the number of basic blocks
		int hue = 360 * id / m_basicBlockCount;
		return Colourise(hue, input);
	}

	// Make up an HSV colour for this item, which we can use to refer to it in jump nodes etc
	// See https://en.wikipedia.org/wiki/HSL_and_HSV#HSV_to_RGB
	// We'll do this by hashing the pointer - not a particularly elegant way to do things, but it works
	int hue = hash_util::hashData((const uint8_t *)&ptr, sizeof(void *), 1234) % 360;

	return Colourise(hue, input);
}

std::string IRPrinter::Colourise(int hue, const std::string &input) {
	int chroma = 255; // Assuming saturation and value are both 1
	int section = hue / 60;
	int minor = hue % 60;
	if (section % 2 == 1)
		minor = 60 - minor;
	minor = (minor * 255) / 60;
	struct {
		int r, g, b;
	} colour;
	switch (section) {
	case 0:
		colour = {chroma, minor, 0};
		break;
	case 1:
		colour = {minor, chroma, 0};
		break;
	case 2:
		colour = {0, chroma, minor};
		break;
	case 3:
		colour = {0, minor, chroma};
		break;
	case 4:
		colour = {minor, 0, chroma};
		break;
	case 5:
		colour = {chroma, 0, minor};
		break;
	}

	return fmt::format("\x1b[38;2;{};{};{}m{}\x1b[m", colour.r, colour.g, colour.b, input);
}
