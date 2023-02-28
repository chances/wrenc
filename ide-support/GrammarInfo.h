//
// Created by znix on 27/02/23.
//

#pragma once

#include "tree_sitter/api.h"

/// This utility class stores information about the grammar, most notably
/// the IDs of different symbols.
class GrammarInfo {
  public:
	GrammarInfo();

	TSSymbol symBlock;
	TSSymbol symVarDecl;
	TSSymbol symIdentifier;
	TSSymbol symVarLoad;
	TSSymbol symClassDef;
	TSSymbol symMethod;
	TSSymbol symMethodForeign;

	TSFieldId fName;

	bool IsMethod(TSSymbol symbol) const;
};
