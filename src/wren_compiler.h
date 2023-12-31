// Derived from the wren source code, src/vm/wren_compiler.h
#pragma once

#include "CompContext.h"
#include "IRNode.h"
#include "Module.h"

// Compiles [source], a string of Wren source code located in [mod], to an
// [IRFn] that contains any top-level (not in function or method) code, or nullptr
// if there is an error in the source code.
//
// If [isExpression] is `true`, [source] should be a single expression, and
// this compiles it to a function that evaluates and returns that expression.
// Otherwise, [source] should be a series of top level statements.
IRFn *wrenCompile(CompContext *context, Module *mod, const char *source, bool isExpression, bool compilingCore);
