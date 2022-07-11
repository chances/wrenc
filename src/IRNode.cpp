//
// Created by znix on 10/07/2022.
//

#include "IRNode.h"

VarDecl::~VarDecl() = default;

std::string IRGlobalDecl::Name() const { return name; }
VarDecl::ScopeType IRGlobalDecl::Scope() const { return SCOPE_MODULE; }

ExprConst::ExprConst() {}
ExprConst::ExprConst(CcValue value) : value(value) {}
