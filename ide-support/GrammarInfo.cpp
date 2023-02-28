//
// Created by znix on 27/02/23.
//

#include "GrammarInfo.h"
#include "ide_devtool.h"

#include <string>

static TSSymbol lookupSym(const std::string &name, bool isNamed = true) {
	const TSLanguage *lang = tree_sitter_wren();
	TSSymbol sym = ts_language_symbol_for_name(lang, name.c_str(), name.length(), isNamed);
	if (sym == 0) {
		fprintf(stderr, "Could not look up Wren grammar symbol: named=%s '%s'\n", isNamed ? "yes" : "no", name.c_str());
		abort();
	}
	return sym;
}

static TSFieldId lookupField(const std::string &name) {
	const TSLanguage *lang = tree_sitter_wren();
	TSFieldId field = ts_language_field_id_for_name(lang, name.c_str(), name.length());
	if (field == 0) {
		fprintf(stderr, "Could not look up Wren grammar field name: '%s'\n", name.c_str());
		abort();
	}
	return field;
}

GrammarInfo::GrammarInfo() {
	symBlock = lookupSym("stmt_block");
	symVarDecl = lookupSym("var_decl");
	symIdentifier = lookupSym("identifier");
	symVarLoad = lookupSym("var_load");

	fName = lookupField("name");
}
