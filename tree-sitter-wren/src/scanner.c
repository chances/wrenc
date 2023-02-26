#include "tree_sitter/parser.h"
#include <assert.h>

enum TokenType { DOT, NEWLINE, STRING, STRING_START, STRING_MID, STRING_END };

static bool readString(TSLexer *lexer, bool inInterpolation, bool permitInterpolation);
static bool readRawString(TSLexer *lexer);

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
static int32_t getChar(TSLexer *lexer) {
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
	//
	// We also handle strings here, because properly handling interpolation
	// and escaping in regexes would be a huge pain.

	// Search for strings. If we can find interpolated strings, normal
	// strings should be fine too!
	if (valid_symbols[STRING_START]) {
		assert(valid_symbols[STRING] && "Strings must be valid if interpolated ones are!");
	}
	if (valid_symbols[STRING]) {
		if (getChar(lexer) == '"') {
			return readString(lexer, false, valid_symbols[STRING_START]);
		}
	}

	// If we can find a middle section of an interpolated string, we should
	// also be able to find it's end.
	assert(valid_symbols[STRING_MID] == valid_symbols[STRING_END]);
	if (valid_symbols[STRING_MID]) {
		if (getChar(lexer) == ')') {
			return readString(lexer, true, true);
		}
	}

	// Try and find a dot in preference to a newline - there's nowhere a dot would fit
	// better than a newline if both are possible.
	if (valid_symbols[DOT]) {
		bool found_newline = false;
		while (true) {
			// Advance through any non-newline whitespace.
			int32_t c = getChar(lexer);

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
		if (getChar(lexer) == '\n') {
			lexer->advance(lexer, false);
			lexer->result_symbol = NEWLINE;
			return true;
		}
	}

	return false;
}

// String handling implementation

static bool readString(TSLexer *lexer, bool inInterpolation, bool permitInterpolation) {
	// Accept the first quote (or closing bracket, if we just finished an
	// interpolated expression).
	lexer->advance(lexer, false);

	bool isStringStart = true;
	bool isEscaped = false;

	while (true) {
		// Check for the end-of-string quote.
		if (lexer->lookahead == '"' && !isEscaped) {
			// Accept the quote
			lexer->advance(lexer, false);

			// Check if there's another quote - if so and we're at the start
			// of the string, this indicates a raw string literal.
			if (isStringStart && lexer->lookahead == '"' && !inInterpolation) {
				// Accept the third quote
				lexer->advance(lexer, false);

				return readRawString(lexer);
			}

			break;
		}

		// If we hit EOF, reject this unterminated string as not being valid.
		if (lexer->eof(lexer)) {
			return false;
		}

		bool thisIsEscape = false;

		// Escapes prevent the next character from having any significance.
		if (!isEscaped) {
			if (lexer->lookahead == '\\') {
				thisIsEscape = true;
			}

			// String interpolation
			if (lexer->lookahead == '%') {
				if (!permitInterpolation)
					return false;

				// Accept the percent, and check it's followed by a bracket
				lexer->advance(lexer, false);

				if (lexer->lookahead == '(') {
					lexer->advance(lexer, false);
					lexer->result_symbol = inInterpolation ? STRING_MID : STRING_START;
					return true;
				}

				// If not, there's no interpolation. It's an error, but don't
				// say the string doesn't exist in that case.
			}
		}

		isStringStart = false;
		isEscaped = thisIsEscape;

		// Accept the character
		lexer->advance(lexer, false);
	}

	lexer->result_symbol = inInterpolation ? STRING_END : STRING;
	return true;
}

static bool readRawString(TSLexer *lexer) {
	// Copied and modified from wren_compiler.
	// The first three quotes have already been consumed.

	// We need to match groups of three characters, so store the last three
	// characters we've parsed. The current last three are the opening quotes, so
	// use spaces as dummy characters to avoid another quote ending the string.
	char c1 = ' ', c2 = ' ', c3 = ' ';

	for (;;) {
		c1 = c2;
		c2 = c3;
		c3 = lexer->lookahead;
		lexer->advance(lexer, false);

		if (c1 == '"' && c2 == '"' && c3 == '"')
			break;

		// Is this an unterminated string?
		if (lexer->eof(lexer)) {
			return false;
		}
	}

	// We've already consumed all three closing quotes.
	lexer->result_symbol = STRING;
	return true;
}
