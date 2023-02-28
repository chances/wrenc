//
// Created by znix on 27/02/23.
//

#pragma once

#include "tree_sitter/api.h"

class GrammarInfo;
extern GrammarInfo *grammarInfo;

// This is inside the generated tree-sitter C file.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" const TSLanguage *tree_sitter_wren(void);
