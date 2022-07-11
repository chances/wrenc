// Derived from the wren source code, src/vm/wren_compiler.h
#pragma once

#include "IRNode.h"
#include "Module.h"
#include "CompContext.h"

// Compiles [source], a string of Wren source code located in [module], to an
// [ObjFn] that contains any top-level (not in function or method) code.
//
// If [isExpression] is `true`, [source] should be a single expression, and
// this compiles it to a function that evaluates and returns that expression.
// Otherwise, [source] should be a series of top level statements.
IRFn *wrenCompile(CompContext *context, Module *module, const char *source, bool isExpression);
