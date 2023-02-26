#include "tree_sitter/parser.h"

enum TokenType { DOT, NEWLINE };

void *tree_sitter_wren_external_scanner_create() { return NULL; }

void tree_sitter_wren_external_scanner_destroy(void *payload) {
	// We didn't allocate anything in create().
}

unsigned tree_sitter_wren_external_scanner_serialize(void *payload, char *buffer) {
	// We don't carry any state.
	return 0;
}

void tree_sitter_wren_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
	// We don't have any state to restore.
}

// Get the lookahead character from the lexer, ignoring whitespace.
static int32_t get_char(TSLexer *lexer) {
	// Always ignore non-newline whitespace
	while (lexer->lookahead == ' ' || lexer->lookahead == '\t' || lexer->lookahead == '\r') {
		lexer->advance(lexer, true);
	}

	return lexer->lookahead;
}

bool tree_sitter_wren_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
	// Process newlines and dots. The whole point of this external scanner is to handle
	// the case of some code like this:
	//
	// var a = b
	//         .c()
	//
	// It is valid Wren code, but it doesn't fit very nicely into the LR(1) scanner.
	// Thus we handle it, and just make the dot swallow up the newlines.

	// Try and find a dot in preference to a newline - there's nowhere a dot would fit
	// better than a newline if both are possible.
	if (valid_symbols[DOT]) {
		bool found_newline = false;
		while (true) {
			// Advance through any non-newline whitespace.
			int32_t c = get_char(lexer);

			// Dots are exactly what we're looking for.
			if (c == '.') {
				// Check if there's a second dot (which if for number ranges),
				// as that's a separate token that doesn't have this special
				// newline rule.
				// Mark the end so that we don't include the dot if there turns
				// out to be a second one after it.
				lexer->mark_end(lexer);
				lexer->advance(lexer, false);

				// Fall back to our newline.
				if (lexer->lookahead == '.')
					break;

				// Move up the end to include the dot we just advanced over.
				lexer->mark_end(lexer);

				lexer->result_symbol = DOT;
				return true;
			}

			// Skip over newlines, but if we're not able to find anything we'll return
			// them as the token.
			if (c == '\n') {
				lexer->advance(lexer, false);
				found_newline = true;
				continue;
			}

			// If we hit something other than a newline or dot, stop here.
			break;
		}

		// We didn't find a dot, but did we find a newline?
		if (found_newline) {
			// (note we already advanced over it, don't do it again here.)
			lexer->result_symbol = NEWLINE;
			return true;
		}
		return false;
	}

	// Search for a single newline
	if (valid_symbols[NEWLINE]) {
		if (get_char(lexer) == '\n') {
			lexer->advance(lexer, false);
			lexer->result_symbol = NEWLINE;
			return true;
		}
	}

	return false;
}
