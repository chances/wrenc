// Derived from the wren source code, src/vm/wren_compiler.h

#include <algorithm>
#include <codecvt>
#include <cstdlib>
#include <errno.h>
#include <fmt/format.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unordered_map>
#include <vector>

#include "wren_compiler.h"

#include "ArenaAllocator.h"
#include "AttributePack.h"
#include "CcValue.h"
#include "ClassInfo.h"
#include "CompContext.h"
#include "IRNode.h"
#include "Module.h"
#include "Scope.h"
#include "SymbolTable.h"

// This is written in bottom-up order, so the tokenization comes first, then
// parsing/code generation. This minimizes the number of explicit forward
// declarations needed.

// The maximum number of local (i.e. not module level) variables that can be
// declared in a single function, method, or chunk of top level code. This is
// the maximum number of variables in scope at one time, and spans block scopes.
//
// Note that this limitation is also explicit in the bytecode. Since
// `CODE_LOAD_LOCAL` and `CODE_STORE_LOCAL` use a single argument byte to
// identify the local, only 256 can be in scope at one time.
#define MAX_LOCALS 256

// The maximum name of a method, not including the signature. This is an
// arbitrary but enforced maximum just so we know how long the method name
// strings need to be in the parser.
#define MAX_METHOD_NAME 64

// The maximum number of upvalues (i.e. variables from enclosing functions)
// that a function can close over.
#define MAX_UPVALUES 256

#define MAX_VARIABLE_NAME 64

// The maximum number of fields a class can have, including inherited fields.
// There isn't any limitation here in wrenc, it's to match intepreted wren.
#define MAX_FIELDS 255

// The maximum depth that interpolation can nest. For example, this string has
// three levels:
//
//      "outside %(one + "%(two + "%(three)")")"
#define MAX_INTERPOLATION_NESTING 8

// The maximum length of a method signature. Signatures look like:
//
//     foo        // Getter.
//     foo()      // No-argument method.
//     foo(_)     // One-argument method.
//     foo(_,_)   // Two-argument method.
//     init foo() // Constructor initializer.
//
// The maximum signature length takes into account the longest method name, the
// maximum number of parameters with separators between them, "init ", and "()".
#define MAX_METHOD_SIGNATURE (MAX_METHOD_NAME + (MAX_PARAMETERS * 2) + 6)

// The maximum number of arguments that can be passed to a method. Note that
// this limitation is hardcoded in other places in the VM, in particular, the
// `CODE_CALL_XX` instructions assume a certain maximum number.
#define MAX_PARAMETERS 16

enum TokenType {
	TOKEN_LEFT_PAREN,
	TOKEN_RIGHT_PAREN,
	TOKEN_LEFT_BRACKET,
	TOKEN_RIGHT_BRACKET,
	TOKEN_LEFT_BRACE,
	TOKEN_RIGHT_BRACE,
	TOKEN_COLON,
	TOKEN_DOT,
	TOKEN_DOTDOT,
	TOKEN_DOTDOTDOT,
	TOKEN_COMMA,
	TOKEN_STAR,
	TOKEN_SLASH,
	TOKEN_PERCENT,
	TOKEN_HASH,
	TOKEN_PLUS,
	TOKEN_MINUS,
	TOKEN_LTLT,
	TOKEN_GTGT,
	TOKEN_PIPE,
	TOKEN_PIPEPIPE,
	TOKEN_CARET,
	TOKEN_AMP,
	TOKEN_AMPAMP,
	TOKEN_BANG,
	TOKEN_TILDE,
	TOKEN_QUESTION,
	TOKEN_EQ,
	TOKEN_LT,
	TOKEN_GT,
	TOKEN_LTEQ,
	TOKEN_GTEQ,
	TOKEN_EQEQ,
	TOKEN_BANGEQ,

	TOKEN_BREAK,
	TOKEN_CONTINUE,
	TOKEN_CLASS,
	TOKEN_CONSTRUCT,
	TOKEN_ELSE,
	TOKEN_FALSE,
	TOKEN_FOR,
	TOKEN_FOREIGN,
	TOKEN_IF,
	TOKEN_IMPORT,
	TOKEN_AS,
	TOKEN_IN,
	TOKEN_IS,
	TOKEN_NULL,
	TOKEN_RETURN,
	TOKEN_STATIC,
	TOKEN_SUPER,
	TOKEN_THIS,
	TOKEN_TRUE,
	TOKEN_VAR,
	TOKEN_WHILE,

	TOKEN_FIELD,
	TOKEN_STATIC_FIELD,
	TOKEN_NAME,
	TOKEN_NUMBER,

	// A string literal without any interpolation, or the last section of a
	// string following the last interpolated expression.
	TOKEN_STRING,

	// A portion of a string literal preceding an interpolated expression. This
	// string:
	//
	//     "a %(b) c %(d) e"
	//
	// is tokenized to:
	//
	//     TOKEN_INTERPOLATION "a "
	//     TOKEN_NAME          b
	//     TOKEN_INTERPOLATION " c "
	//     TOKEN_NAME          d
	//     TOKEN_STRING        " e"
	TOKEN_INTERPOLATION,

	TOKEN_LINE,

	TOKEN_ERROR,
	TOKEN_EOF
};

struct Token {
	TokenType type;

	// The full contents of the token.
	// In regular Wren this is a pointer into the source code and a length field to reduce copies. In C++ it's
	// easier to use a std::string and just waste a little time copying it.
	std::string contents;

	// The 1-based line where the token appears.
	int line;

	// The parsed value if the token is a literal.
	CcValue value;

	IRDebugInfo MakeDebugInfo() const;
};

struct Parser {
	CompContext *context;

	// The module being parsed.
	Module *targetModule;

	// The source code being parsed.
	const char *source;

	// The beginning of the currently-being-lexed token in [source].
	const char *tokenStart;

	// The current character being lexed in [source].
	const char *currentChar;

	// The 1-based line number of [currentChar].
	int currentLine;

	// The upcoming token.
	Token next;

	// The most recently lexed token.
	Token current;

	// The most recently consumed/advanced token.
	Token previous;

	// Tracks the lexing state when tokenizing interpolated strings.
	//
	// Interpolated strings make the lexer not strictly regular: we don't know
	// whether a ")" should be treated as a RIGHT_PAREN token or as ending an
	// interpolated expression unless we know whether we are inside a string
	// interpolation and how many unmatched "(" there are. This is particularly
	// complex because interpolation can nest:
	//
	//     " %( " %( inner ) " ) "
	//
	// This tracks that state. The parser maintains a stack of ints, one for each
	// level of current interpolation nesting. Each value is the number of
	// unmatched "(" that are waiting to be closed.
	int parens[MAX_INTERPOLATION_NESTING];
	int numParens;

	// Whether compile errors should be printed to stderr or discarded.
	bool printErrors;

	// If a syntax or compile error has occurred.
	bool hasError;

	// Whether we're allowed to compile wrenc-internal stuff, as part of building our
	// runtime library.
	bool compilingInternal = false;
};

// Bookkeeping information for the current loop being compiled.
struct Loop {
	// The label that the end of loop should jump back to.
	StmtLabel *start = nullptr;

	// Jumps that exit the loop. Once we're done building the loop, all these jumps
	// will have their targets set to a label just after the end of the loop.
	std::vector<StmtJump *> exitJumps;

	// Index of the first instruction of the body of the loop.
	StmtLabel *body = nullptr;

	// The index of the stack frame that the body of the loop is in. If the loop is
	// broken out of, this is used to clean up all the variables inside the loop.
	int contentFrameId = -1;

	// The loop enclosing this one, or NULL if this is the outermost loop.
	Loop *enclosing;
};

class Compiler {
  public:
	Parser *parser;

	ScopeStack locals;

	// The compiler for the function enclosing this one, or NULL if it's the
	// top level.
	Compiler *parent;

	// The current level of block scope nesting, where zero is no nesting. A -1
	// here means top-level code is being compiled and there is no block scope
	// in effect at all. Any variables declared will be module-level.
	int scopeDepth;

	// The current innermost loop being compiled, or NULL if not in a loop.
	Loop *loop;

	// If this is a compiler for a method, keeps track of the class enclosing it.
	// Note this is not set on the compiler that compiles the body of a method - it's
	// set on it's parent, the one parsing the block that contains the class declaration.
	ClassInfo *enclosingClass;

	// The function being compiled.
	IRFn *fn;

	// The local variables associated with the parameters. These should be immediately
	// initialised with the SSA parameter variables (found in IRFn).
	std::vector<LocalVariable *> parameterLocals;

	// Whether or not the compiler is for a constructor initializer
	bool isInitializer;

	// Utility functions:
	template <typename T, typename... Args> T *New(Args &&...args) {
		T *thing = parser->context->alloc.New<T, Args...>(std::forward<Args>(args)...);

		// Set the debug info for nodes automatically.
		// There's certainly a more efficient way to do this that avoids the dynamic_cast, but we don't care.
		IRNode *baseNode = dynamic_cast<IRNode *>(thing);
		if (baseNode)
			baseNode->debugInfo = parser->previous.MakeDebugInfo();

		return thing;
	}

	/// Creates and adds a new statement
	/// This is just shorthand for creating the statement with compiler->New and then adding it to the block.
	template <typename T, typename... Args> T *AddNew(StmtBlock *block, Args &&...args) {
		T *value = New<T, Args...>(std::forward<Args>(args)...);
		block->Add(value);
		return value;
	}
};

// Forward declarations

static IRExpr *null(Compiler *compiler, bool canAssign = false);
static ClassInfo *getEnclosingClass(Compiler *compiler);

// The stack effect of each opcode. The index in the array is the opcode, and
// the value is the stack effect of that instruction.
// static const int stackEffects[] = {
// #define OPCODE(_, effect) effect,
// #include "wren_opcodes.h"
// #undef OPCODE
// };

#define ASSERT(cond, msg)                                                                                              \
	do {                                                                                                               \
		bool assert_macro_cond = (cond);                                                                               \
		if (!assert_macro_cond)                                                                                        \
			assertionFailure(__FILE__, __LINE__, (msg));                                                               \
	} while (0)

static void assertionFailure(const char *file, int line, const char *msg) {
	fmt::print(stderr, "Assertion failure at {}:{} - {}\n", file, line, msg);
	abort();
}

static std::string moduleName(Parser *parser) {
	std::optional<std::string> mod = parser->targetModule->Name();
	return mod ? mod.value() : "<unknown>";
}

static void printError(Parser *parser, int line, const char *label, const char *format, va_list args) {
	parser->hasError = true;
	if (!parser->printErrors)
		return;

	// Format the label and message.
	char buf[256];
	vsnprintf(buf, sizeof(buf), format, args);
	std::string message = std::string(label) + ": " + std::string(buf);

	fmt::print(stderr, "WrenCC at {}:{} - {}\n", moduleName(parser), line, message);
}

// Outputs a lexical error.
static void lexError(Parser *parser, const char *format, ...) {
	va_list args;
	va_start(args, format);
	printError(parser, parser->currentLine, "Error", format, args);
	va_end(args);
}

// Outputs a compile or syntax error. This also marks the compilation as having
// an error, which ensures that the resulting code will be discarded and never
// run. This means that after calling error(), it's fine to generate whatever
// invalid bytecode you want since it won't be used.
//
// You'll note that most places that call error() continue to parse and compile
// after that. That's so that we can try to find as many compilation errors in
// one pass as possible instead of just bailing at the first one.
static void error(Compiler *compiler, const char *format, ...) {
	Token *token = &compiler->parser->previous;

	// If the parse error was caused by an error token, the lexer has already
	// reported it.
	if (token->type == TOKEN_ERROR)
		return;

	va_list args;
	va_start(args, format);
	if (token->type == TOKEN_LINE) {
		printError(compiler->parser, token->line, "Error at newline", format, args);
	} else if (token->type == TOKEN_EOF) {
		printError(compiler->parser, token->line, "Error at end of file", format, args);
	} else {
		std::string label = fmt::format("Error at '{}'", token->contents);
		printError(compiler->parser, token->line, label.c_str(), format, args);
	}
	va_end(args);
}

// Initializes [compiler].
static void initCompiler(Compiler *compiler, Parser *parser, Compiler *parent) {
	compiler->parser = parser;
	compiler->parent = parent;
	compiler->loop = NULL;
	compiler->enclosingClass = NULL;
	compiler->isInitializer = false;
	compiler->fn = NULL;

	parser->context->compiler = compiler;

	if (parent == NULL) {
		// Compiling top-level code, so the initial scope is module-level.
		compiler->scopeDepth = -1;
	} else {
		// The initial scope for functions and methods is local scope.
		compiler->scopeDepth = 0;
	}

	compiler->fn = compiler->New<IRFn>();

	// Push the root-level stack frame
	compiler->fn->rootBeginUpvalues = compiler->New<StmtBeginUpvalues>();
	compiler->locals.PushFrame(compiler->fn->rootBeginUpvalues);
}

// Lexing ----------------------------------------------------------------------

typedef struct {
	const char *identifier;
	size_t length;
	TokenType tokenType;
} Keyword;

// The table of reserved words and their associated token types.
static Keyword keywords[] = {
    // clang-format off
    {"break", 5, TOKEN_BREAK},
    {"continue", 8, TOKEN_CONTINUE},
    {"class", 5, TOKEN_CLASS},
    {"construct", 9, TOKEN_CONSTRUCT},
    {"else", 4, TOKEN_ELSE},
    {"false", 5, TOKEN_FALSE},
    {"for", 3, TOKEN_FOR},
    {"foreign", 7, TOKEN_FOREIGN},
    {"if", 2, TOKEN_IF},
    {"import", 6, TOKEN_IMPORT},
    {"as", 2, TOKEN_AS},
    {"in", 2, TOKEN_IN},
    {"is", 2, TOKEN_IS},
    {"null", 4, TOKEN_NULL},
    {"return", 6, TOKEN_RETURN},
    {"static", 6, TOKEN_STATIC},
    {"super", 5, TOKEN_SUPER},
    {"this", 4, TOKEN_THIS},
    {"true", 4, TOKEN_TRUE},
    {"var", 3, TOKEN_VAR},
    {"while", 5, TOKEN_WHILE},
    {NULL, 0, TOKEN_EOF} // Sentinel to mark the end of the array.
    // clang-format on
};

// Returns true if [c] is a valid (non-initial) identifier character.
static bool isName(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

// Returns true if [c] is a digit.
static bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Returns the current character the parser is sitting on.
static char peekChar(Parser *parser) { return *parser->currentChar; }

// Returns the character after the current character.
static char peekNextChar(Parser *parser) {
	// If we're at the end of the source, don't read past it.
	if (peekChar(parser) == '\0')
		return '\0';
	return *(parser->currentChar + 1);
}

// Advances the parser forward one character.
static char nextChar(Parser *parser) {
	char c = peekChar(parser);
	parser->currentChar++;
	if (c == '\n')
		parser->currentLine++;
	return c;
}

// If the current character is [c], consumes it and returns `true`.
static bool matchChar(Parser *parser, char c) {
	if (peekChar(parser) != c)
		return false;
	nextChar(parser);
	return true;
}

// Sets the parser's current token to the given [type] and current character
// range.
static void makeToken(Parser *parser, TokenType type) {
	parser->next.type = type;
	parser->next.contents = std::string(parser->tokenStart, parser->currentChar);
	parser->next.line = parser->currentLine;

	// Make line tokens appear on the line containing the "\n".
	if (type == TOKEN_LINE)
		parser->next.line--;
}

IRDebugInfo Token::MakeDebugInfo() const {
	return IRDebugInfo{
	    .lineNumber = line,
	    // TODO column
	};
}

// If the current character is [c], then consumes it and makes a token of type
// [two]. Otherwise makes a token of type [one].
static void twoCharToken(Parser *parser, char c, TokenType two, TokenType one) {
	makeToken(parser, matchChar(parser, c) ? two : one);
}

// Skips the rest of the current line.
static void skipLineComment(Parser *parser) {
	while (peekChar(parser) != '\n' && peekChar(parser) != '\0') {
		nextChar(parser);
	}
}

// Skips the rest of a block comment.
static void skipBlockComment(Parser *parser) {
	int nesting = 1;
	while (nesting > 0) {
		if (peekChar(parser) == '\0') {
			lexError(parser, "Unterminated block comment.");
			return;
		}

		if (peekChar(parser) == '/' && peekNextChar(parser) == '*') {
			nextChar(parser);
			nextChar(parser);
			nesting++;
			continue;
		}

		if (peekChar(parser) == '*' && peekNextChar(parser) == '/') {
			nextChar(parser);
			nextChar(parser);
			nesting--;
			continue;
		}

		// Regular comment character.
		nextChar(parser);
	}
}

// Reads the next character, which should be a hex digit (0-9, a-f, or A-F) and
// returns its numeric value. If the character isn't a hex digit, returns -1.
static int readHexDigit(Parser *parser) {
	char c = nextChar(parser);
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;

	// Don't consume it if it isn't expected. Keeps us from reading past the end
	// of an unterminated string.
	parser->currentChar--;
	return -1;
}

// Parses the numeric value of the current token.
static void makeNumber(Parser *parser, bool isHex) {
	errno = 0;

	if (isHex) {
		parser->next.value = (double)strtoll(parser->tokenStart, NULL, 16);
	} else {
		parser->next.value = strtod(parser->tokenStart, NULL);
	}

	if (errno == ERANGE) {
		lexError(parser, "Number literal was too large (%d).", sizeof(long int));
		parser->next.value = 0;
	}

	// We don't check that the entire token is consumed after calling strtoll()
	// or strtod() because we've already scanned it ourselves and know it's valid.

	makeToken(parser, TOKEN_NUMBER);
}

// Finishes lexing a hexadecimal number literal.
static void readHexNumber(Parser *parser) {
	// Skip past the `x` used to denote a hexadecimal literal.
	nextChar(parser);

	// Iterate over all the valid hexadecimal digits found.
	while (readHexDigit(parser) != -1)
		continue;

	makeNumber(parser, true);
}

// Finishes lexing a number literal.
static void readNumber(Parser *parser) {
	while (isDigit(peekChar(parser)))
		nextChar(parser);

	// See if it has a floating point. Make sure there is a digit after the "."
	// so we don't get confused by method calls on number literals.
	if (peekChar(parser) == '.' && isDigit(peekNextChar(parser))) {
		nextChar(parser);
		while (isDigit(peekChar(parser)))
			nextChar(parser);
	}

	// See if the number is in scientific notation.
	if (matchChar(parser, 'e') || matchChar(parser, 'E')) {
		// Allow a single positive/negative exponent symbol.
		if (!matchChar(parser, '+')) {
			matchChar(parser, '-');
		}

		if (!isDigit(peekChar(parser))) {
			lexError(parser, "Unterminated scientific notation.");
		}

		while (isDigit(peekChar(parser)))
			nextChar(parser);
	}

	makeNumber(parser, false);
}

// Finishes lexing an identifier. Handles reserved words.
static void readName(Parser *parser, TokenType type, char firstChar) {
	std::string string;
	string.push_back(firstChar);

	while (isName(peekChar(parser)) || isDigit(peekChar(parser))) {
		char c = nextChar(parser);
		string.push_back(c);
	}

	// Update the type if it's a keyword.
	size_t length = parser->currentChar - parser->tokenStart;
	for (int i = 0; keywords[i].identifier != NULL; i++) {
		if (length == keywords[i].length && memcmp(parser->tokenStart, keywords[i].identifier, length) == 0) {
			type = keywords[i].tokenType;
			break;
		}
	}

	parser->next.value = string;

	makeToken(parser, type);
}

// Reads [digits] hex digits in a string literal and returns their number value.
static int readHexEscape(Parser *parser, int digits, const char *description) {
	int value = 0;
	for (int i = 0; i < digits; i++) {
		if (peekChar(parser) == '"' || peekChar(parser) == '\0') {
			lexError(parser, "Incomplete %s escape sequence.", description);

			// Don't consume it if it isn't expected. Keeps us from reading past the
			// end of an unterminated string.
			parser->currentChar--;
			break;
		}

		int digit = readHexDigit(parser);
		if (digit == -1) {
			lexError(parser, "Invalid %s escape sequence.", description);
			break;
		}

		value = (value * 16) | digit;
	}

	return value;
}

// Reads a hex digit Unicode escape sequence in a string literal.
static void readUnicodeEscape(Parser *parser, std::string &string, int length) {
	char32_t value = readHexEscape(parser, length, "Unicode");

	// Convert the single UTF-32 character to a UTF-8 string.
	std::codecvt_utf8<char32_t> conv;
	std::vector<char> utf8;
	utf8.resize(conv.max_length());

	std::mbstate_t state;
	const char32_t *unused;
	char *utf8next = nullptr;
	conv.out(state, &value, &value + 1, unused, utf8.data(), utf8.data() + utf8.size(), utf8next);

	// Chop off the unused part
	int utf8length = utf8next - utf8.data();
	utf8.resize(utf8length);

	string.insert(string.end(), utf8.begin(), utf8.end());
}

static void readRawString(Parser *parser) {
	std::string string;
	TokenType type = TOKEN_STRING;

	// consume the second and third "
	nextChar(parser);
	nextChar(parser);

	int skipStart = 0;
	int firstNewline = -1;

	int skipEnd = -1;
	int lastNewline = -1;

	for (;;) {
		char c = nextChar(parser);
		char c1 = peekChar(parser);
		char c2 = peekNextChar(parser);

		if (c == '\r')
			continue;

		if (c == '\n') {
			lastNewline = string.size();
			skipEnd = lastNewline;
			firstNewline = firstNewline == -1 ? string.size() : firstNewline;
		}

		if (c == '"' && c1 == '"' && c2 == '"')
			break;

		bool isWhitespace = c == ' ' || c == '\t';
		skipEnd = c == '\n' || isWhitespace ? skipEnd : -1;

		// If we haven't seen a newline or other character yet,
		// and still seeing whitespace, count the characters
		// as skippable till we know otherwise
		bool skippable = skipStart != -1 && isWhitespace && firstNewline == -1;
		skipStart = skippable ? string.size() + 1 : skipStart;

		// We've counted leading whitespace till we hit something else,
		// but it's not a newline, so we reset skipStart since we need these characters
		if (firstNewline == -1 && !isWhitespace && c != '\n')
			skipStart = -1;

		if (c == '\0' || c1 == '\0' || c2 == '\0') {
			lexError(parser, "Unterminated raw string.");

			// Don't consume it if it isn't expected. Keeps us from reading past the
			// end of an unterminated string.
			parser->currentChar--;
			break;
		}

		string.push_back(c);
	}

	// consume the second and third "
	nextChar(parser);
	nextChar(parser);

	int offset = 0;
	int count = string.size();

	if (firstNewline != -1 && skipStart == firstNewline)
		offset = firstNewline + 1;

	if (lastNewline != -1 && skipEnd == lastNewline)
		count = lastNewline;

	count -= (offset > count) ? count : offset;

	if (offset > (int)string.size()) {
		parser->next.value = "";
	} else {
		parser->next.value = string.substr(offset, count);
	}

	makeToken(parser, type);
}

// Finishes lexing a string literal.
static void readString(Parser *parser) {
	std::string string;
	TokenType type = TOKEN_STRING;

	for (;;) {
		char c = nextChar(parser);
		if (c == '"')
			break;
		if (c == '\r')
			continue;

		if (c == '\0') {
			lexError(parser, "Unterminated string.");

			// Don't consume it if it isn't expected. Keeps us from reading past the
			// end of an unterminated string.
			parser->currentChar--;
			break;
		}

		if (c == '%') {
			if (parser->numParens < MAX_INTERPOLATION_NESTING) {
				// TODO: Allow format string.
				if (nextChar(parser) != '(')
					lexError(parser, "Expect '(' after '%%'.");

				parser->parens[parser->numParens++] = 1;
				type = TOKEN_INTERPOLATION;
				break;
			}

			lexError(parser, "Interpolation may only nest %d levels deep.", MAX_INTERPOLATION_NESTING);
		}

		if (c == '\\') {
			switch (nextChar(parser)) {
			case '"':
				string.push_back('"');
				break;
			case '\\':
				string.push_back('\\');
				break;
			case '%':
				string.push_back('%');
				break;
			case '0':
				string.push_back('\0');
				break;
			case 'a':
				string.push_back('\a');
				break;
			case 'b':
				string.push_back('\b');
				break;
			case 'e':
				string.push_back('\33');
				break;
			case 'f':
				string.push_back('\f');
				break;
			case 'n':
				string.push_back('\n');
				break;
			case 'r':
				string.push_back('\r');
				break;
			case 't':
				string.push_back('\t');
				break;
			case 'u':
				readUnicodeEscape(parser, string, 4);
				break;
			case 'U':
				readUnicodeEscape(parser, string, 8);
				break;
			case 'v':
				string.push_back('\v');
				break;
			case 'x':
				string.push_back((char)readHexEscape(parser, 2, "byte"));
				break;

			default:
				lexError(parser, "Invalid escape character '%c'.", *(parser->currentChar - 1));
				break;
			}
		} else {
			string.push_back(c);
		}
	}

	parser->next.value = string;

	makeToken(parser, type);
}

// Lex the next token and store it in [parser.next].
static void nextToken(Parser *parser) {
	parser->previous = parser->current;
	parser->current = parser->next;

	// If we are out of tokens, don't try to tokenize any more. We *do* still
	// copy the TOKEN_EOF to previous so that code that expects it to be consumed
	// will still work.
	if (parser->next.type == TOKEN_EOF)
		return;
	if (parser->current.type == TOKEN_EOF)
		return;

	while (peekChar(parser) != '\0') {
		parser->tokenStart = parser->currentChar;

		char c = nextChar(parser);
		switch (c) {
		case '(':
			// If we are inside an interpolated expression, count the unmatched "(".
			if (parser->numParens > 0)
				parser->parens[parser->numParens - 1]++;
			makeToken(parser, TOKEN_LEFT_PAREN);
			return;

		case ')':
			// If we are inside an interpolated expression, count the ")".
			if (parser->numParens > 0 && --parser->parens[parser->numParens - 1] == 0) {
				// This is the final ")", so the interpolation expression has ended.
				// This ")" now begins the next section of the template string.
				parser->numParens--;
				readString(parser);
				return;
			}

			makeToken(parser, TOKEN_RIGHT_PAREN);
			return;

		case '[':
			makeToken(parser, TOKEN_LEFT_BRACKET);
			return;
		case ']':
			makeToken(parser, TOKEN_RIGHT_BRACKET);
			return;
		case '{':
			makeToken(parser, TOKEN_LEFT_BRACE);
			return;
		case '}':
			makeToken(parser, TOKEN_RIGHT_BRACE);
			return;
		case ':':
			makeToken(parser, TOKEN_COLON);
			return;
		case ',':
			makeToken(parser, TOKEN_COMMA);
			return;
		case '*':
			makeToken(parser, TOKEN_STAR);
			return;
		case '%':
			makeToken(parser, TOKEN_PERCENT);
			return;
		case '#': {
			// Ignore shebang on the first line.
			if (parser->currentLine == 1 && peekChar(parser) == '!' && peekNextChar(parser) == '/') {
				skipLineComment(parser);
				break;
			}
			// Otherwise we treat it as a token
			makeToken(parser, TOKEN_HASH);
			return;
		}
		case '^':
			makeToken(parser, TOKEN_CARET);
			return;
		case '+':
			makeToken(parser, TOKEN_PLUS);
			return;
		case '-':
			makeToken(parser, TOKEN_MINUS);
			return;
		case '~':
			makeToken(parser, TOKEN_TILDE);
			return;
		case '?':
			makeToken(parser, TOKEN_QUESTION);
			return;

		case '|':
			twoCharToken(parser, '|', TOKEN_PIPEPIPE, TOKEN_PIPE);
			return;
		case '&':
			twoCharToken(parser, '&', TOKEN_AMPAMP, TOKEN_AMP);
			return;
		case '=':
			twoCharToken(parser, '=', TOKEN_EQEQ, TOKEN_EQ);
			return;
		case '!':
			twoCharToken(parser, '=', TOKEN_BANGEQ, TOKEN_BANG);
			return;

		case '.':
			if (matchChar(parser, '.')) {
				twoCharToken(parser, '.', TOKEN_DOTDOTDOT, TOKEN_DOTDOT);
				return;
			}

			makeToken(parser, TOKEN_DOT);
			return;

		case '/':
			if (matchChar(parser, '/')) {
				skipLineComment(parser);
				break;
			}

			if (matchChar(parser, '*')) {
				skipBlockComment(parser);
				break;
			}

			makeToken(parser, TOKEN_SLASH);
			return;

		case '<':
			if (matchChar(parser, '<')) {
				makeToken(parser, TOKEN_LTLT);
			} else {
				twoCharToken(parser, '=', TOKEN_LTEQ, TOKEN_LT);
			}
			return;

		case '>':
			if (matchChar(parser, '>')) {
				makeToken(parser, TOKEN_GTGT);
			} else {
				twoCharToken(parser, '=', TOKEN_GTEQ, TOKEN_GT);
			}
			return;

		case '\n':
			makeToken(parser, TOKEN_LINE);
			return;

		case ' ':
		case '\r':
		case '\t':
			// Skip forward until we run out of whitespace.
			while (peekChar(parser) == ' ' || peekChar(parser) == '\r' || peekChar(parser) == '\t') {
				nextChar(parser);
			}
			break;

		case '"': {
			if (peekChar(parser) == '"' && peekNextChar(parser) == '"') {
				readRawString(parser);
				return;
			}
			readString(parser);
			return;
		}
		case '_':
			readName(parser, peekChar(parser) == '_' ? TOKEN_STATIC_FIELD : TOKEN_FIELD, c);
			return;

		case '0':
			if (peekChar(parser) == 'x') {
				readHexNumber(parser);
				return;
			}

			readNumber(parser);
			return;

		default:
			if (isName(c)) {
				readName(parser, TOKEN_NAME, c);
			} else if (isDigit(c)) {
				readNumber(parser);
			} else {
				if (c >= 32 && c <= 126) {
					lexError(parser, "Invalid character '%c'.", c);
				} else {
					// Don't show non-ASCII values since we didn't UTF-8 decode the
					// bytes. Since there are no non-ASCII byte values that are
					// meaningful code units in Wren, the lexer works on raw bytes,
					// even though the source code and console output are UTF-8.
					lexError(parser, "Invalid byte 0x%x.", (uint8_t)c);
				}
				parser->next.type = TOKEN_ERROR;
				parser->next.contents.clear();
			}
			return;
		}
	}

	// If we get here, we're out of source, so just make EOF tokens.
	parser->tokenStart = parser->currentChar;
	makeToken(parser, TOKEN_EOF);
}

// Parsing ---------------------------------------------------------------------

// Returns `true` if [name] is a local variable name (starts with a lowercase letter).
// Copied from wren_vm.h
static inline bool wrenIsLocalName(const std::string &name) {
	return !name.empty() && name[0] >= 'a' && name[0] <= 'z';
}

// Returns the type of the current token.
static TokenType peek(Compiler *compiler) { return compiler->parser->current.type; }

// Returns the type of the current token.
static TokenType peekNext(Compiler *compiler) { return compiler->parser->next.type; }

// Consumes the current token if its type is [expected]. Returns true if a
// token was consumed.
static bool match(Compiler *compiler, TokenType expected) {
	if (peek(compiler) != expected)
		return false;

	nextToken(compiler->parser);
	return true;
}

// Consumes the current token. Emits an error if its type is not [expected].
static void consume(Compiler *compiler, TokenType expected, const char *errorMessage) {
	nextToken(compiler->parser);
	if (compiler->parser->previous.type != expected) {
		error(compiler, errorMessage);

		// If the next token is the one we want, assume the current one is just a
		// spurious error and discard it to minimize the number of cascaded errors.
		if (compiler->parser->current.type == expected)
			nextToken(compiler->parser);
	}
}

// Matches one or more newlines. Returns true if at least one was found.
static bool matchLine(Compiler *compiler) {
	if (!match(compiler, TOKEN_LINE))
		return false;

	while (match(compiler, TOKEN_LINE))
		;
	return true;
}

// Discards any newlines starting at the current token.
static void ignoreNewlines(Compiler *compiler) { matchLine(compiler); }

// Consumes the current token. Emits an error if it is not a newline. Then
// discards any duplicate newlines following it.
static void consumeLine(Compiler *compiler, const char *errorMessage) {
	consume(compiler, TOKEN_LINE, errorMessage);
	ignoreNewlines(compiler);
}

static void allowLineBeforeDot(Compiler *compiler) {
	if (peek(compiler) == TOKEN_LINE && peekNext(compiler) == TOKEN_DOT) {
		nextToken(compiler->parser);
	}
}

// Variables and scopes --------------------------------------------------------

// Create a new local variable with [name]. Assumes the current scope is local.
// If the variable is not unique, returns nullptr.
static LocalVariable *addLocal(Compiler *compiler, const std::string &name) {
	LocalVariable *local = compiler->New<LocalVariable>();
	local->name = name;

	if (!compiler->locals.Add(local))
		return nullptr;

	compiler->fn->locals.push_back(local);

	return local;
}

// Create a new temporary (for compiler use only) local variable with the
// name [debugName], though this shouldn't ever be shown to the user, doesn't
// have to be unique and doesn't have to be a valid identifier.
static SSAVariable *addTemporary(Compiler *compiler, const std::string &debugName) {
	SSAVariable *local = compiler->New<SSAVariable>();
	local->name = debugName;
	compiler->fn->temporaries.push_back(local);
	return local;
}

// Add a non-SSA temporary.
static LocalVariable *addMutableTemporary(Compiler *compiler, const std::string &debugName) {
	LocalVariable *local = compiler->New<LocalVariable>();
	local->name = debugName;
	compiler->fn->locals.push_back(local);
	return local;
}

// Declares a variable, from the name of [token].
//
// If [token] is `NULL`, uses the previously consumed token.
static VarDecl *declareVariable(Compiler *compiler, Token *token) {
	if (token == NULL)
		token = &compiler->parser->previous;

	if (token->contents.size() > MAX_VARIABLE_NAME) {
		error(compiler, "Variable name cannot be longer than %d characters.", MAX_VARIABLE_NAME);
	}

	if (compiler->scopeDepth == -1) {
		// Top-level module scope.
		IRGlobalDecl *global = compiler->parser->targetModule->AddVariable(token->contents);

		// If the variable was already defined, check if it was implicitly defined
		if (global == nullptr) {
			IRGlobalDecl *current = compiler->parser->targetModule->FindVariable(token->contents);
			if (current->undeclaredLineUsed.has_value()) {
				// If this was a localname we want to error if it was referenced before this definition.
				// This covers cases like this in the global scope:
				//   System.print(b)
				//   var b = 1
				if (wrenIsLocalName(token->contents)) {
					error(compiler, "Variable '%s' referenced before this definition (first use at line %d).",
					    token->contents.c_str(), current->undeclaredLineUsed.value());
				}

				current->undeclaredLineUsed = std::optional<int>();
				global = current;
			}

			// If it was already explicitly defined, throw an error
			if (global == nullptr) {
				error(compiler, "Module variable is already defined.");
				return current; // Return something, so the compiler can continue
			}
		}

		compiler->parser->targetModule->AddNode(global);

		// } else if (symbol == -3) {
		// }

		return global;
	}

	LocalVariable *var = addLocal(compiler, token->contents);

	// Adding a variable only fails if the variable already exists in the scope. If the variable exists in the
	// parent scope, it's not a problem as we shadow it.
	if (!var) {
		error(compiler, "Variable is already declared in this scope.");

		// Return the other variable, so the compiler can continue
		return compiler->locals.Lookup(token->contents);
	}

	// It's no issue for us to support an arbitrary number of locals, but implementing this to avoid
	// incompatibilities with regular Wren might be nice.
	//
	// if (compiler->numLocals == MAX_LOCALS) {
	// 	error(compiler, "Cannot declare more than %d variables in one scope.", MAX_LOCALS);
	// 	return -1;
	// }

	return var;
}

// Parses a name token and declares a variable in the current scope with that
// name. Returns its declaration.
static VarDecl *declareNamedVariable(Compiler *compiler) {
	consume(compiler, TOKEN_NAME, "Expect variable name.");
	return declareVariable(compiler, NULL);
}

// Stores a variable with the previously defined symbol in the current scope.
static IRNode *defineVariable(Compiler *compiler, VarDecl *var, IRExpr *value) {
	// Store module-level variable
	StmtAssign *stmt = compiler->New<StmtAssign>();
	stmt->var = var;
	stmt->expr = value;
	return stmt;
}

// Starts a new local block scope.
[[nodiscard]] static StmtBeginUpvalues *pushScope(Compiler *compiler) {
	StmtBeginUpvalues *upvalueContainer = compiler->New<StmtBeginUpvalues>();
	compiler->scopeDepth++;
	compiler->locals.PushFrame(upvalueContainer);
	return upvalueContainer;
}

// Generates code to discard local variables at [depth] or greater. Does *not*
// actually undeclare variables or pop any scopes, though. This is called
// directly when compiling "break" statements to ditch the local variables
// before jumping out of the loop even though they are still in scope *past*
// the break instruction.
// Returns the statement required to release these variables, or null if none is required. This is
// mostly for upvalues, to kick them off to the heap if they're still being referenced.
static IRStmt *discardLocals(Compiler *compiler, const std::vector<ScopeFrame *> &discarded) {
	ASSERT(compiler->scopeDepth > -1, "Cannot exit top-level scope.");

	if (discarded.empty())
		return nullptr;

	StmtRelocateUpvalues *relocate = compiler->New<StmtRelocateUpvalues>();
	for (ScopeFrame *frame : discarded) {
		// For the root frame
		if (frame->upvalueContainer == nullptr)
			continue;

		const std::vector<LocalVariable *> &vars = frame->upvalueContainer->variables;
		bool hasUpvalues =
		    std::any_of(vars.begin(), vars.end(), [](LocalVariable *var) { return !var->upvalues.empty(); });

		if (!hasUpvalues)
			continue;
		relocate->upvalueSets.push_back(frame->upvalueContainer);
	}

	if (relocate->upvalueSets.empty())
		return nullptr;

	return relocate;
}

// Closes the last pushed block scope and discards any local variables declared
// in that scope. This should only be called in a statement context where no
// temporaries are still on the stack.
// Returns the statement required to release these variables, or null if none is required. This is
// mostly for upvalues, to kick them off to the heap if they're still being referenced.
static IRStmt *popScope(Compiler *compiler) {
	std::vector<ScopeFrame *> discarded = compiler->locals.GetFramesSince(compiler->locals.GetTopFrame());
	IRStmt *stmt = discardLocals(compiler, discarded);
	compiler->scopeDepth--;
	compiler->locals.PopFrame();
	return stmt;
}

// Adds an upvalue to [compiler]'s function with the given properties. Does not
// add one if an upvalue for that variable is already in the list. Returns the
// index of the upvalue.
static UpvalueVariable *addUpvalue(Compiler *compiler, VarDecl *var) {
	// Look for an existing one.
	auto upvalueIter = compiler->fn->upvalues.find(var);
	if (upvalueIter != compiler->fn->upvalues.end())
		return upvalueIter->second;

	// Otherwise, create one
	UpvalueVariable *upvalue = compiler->New<UpvalueVariable>(var, compiler->fn);
	compiler->fn->upvalues[var] = upvalue;

	if (compiler->fn->upvaluesByName.contains(var->Name())) {
		fmt::print(stderr, "Duplicate upvalue variable name '{}'\n", var->Name().c_str());
		abort();
	}
	compiler->fn->upvaluesByName[var->Name()] = upvalue;

	return upvalue;
}

// Attempts to look up [name] in the functions enclosing the one being compiled
// by [compiler]. If found, it adds an upvalue for it to this compiler's list
// of upvalues (unless it's already in there) and returns its index. If not
// found, returns -1.
//
// If the name is found outside of the immediately enclosing function, this
// will flatten the closure and add upvalues to all of the intermediate
// functions so that it gets walked down to this one.
//
// If it reaches a method boundary, this stops and returns -1 since methods do
// not close over local variables.
static UpvalueVariable *findUpvalue(Compiler *compiler, const std::string &name) {
	// If we are at the top level, we didn't find it. This is because you don't have
	// local variables at the top level - they're all module-level variables.
	if (compiler->parent == NULL)
		return nullptr;

	// If we hit the method boundary (and the name isn't a static field), then
	// stop looking for it. We'll instead treat it as a self send.
	if (name[0] != '_' && compiler->parent->enclosingClass != NULL)
		return nullptr;

	// Check if we've already got an upvalue for this variable
	auto byNameIter = compiler->fn->upvaluesByName.find(name);
	if (byNameIter != compiler->fn->upvaluesByName.end()) {
		return byNameIter->second;
	}

	// See if it's a local variable in the immediately enclosing function.
	LocalVariable *local = compiler->parent->locals.Lookup(name);
	if (local) {
		UpvalueVariable *upvalue = addUpvalue(compiler, local);

		// Mark the local as an upvalue so we know to close it when it goes out of scope.
		local->upvalues.push_back(upvalue);

		return upvalue;
	}

	// See if it's an upvalue in the immediately enclosing function. In other
	// words, if it's a local variable in a non-immediately enclosing function.
	// This "flattens" closures automatically: it adds upvalues to all of the
	// intermediate functions to get from the function where a local is declared
	// all the way into the possibly deeply nested function that is closing over
	// it.
	UpvalueVariable *upvalue = findUpvalue(compiler->parent, name);
	if (upvalue) {
		return addUpvalue(compiler, upvalue);
	}

	// If we got here, we walked all the way up the parent chain and couldn't
	// find it.
	return nullptr;
}

// Look up [name] in the current scope to see what variable it refers to.
// Returns the variable either in local scope, or the enclosing function's
// upvalue list. Does not search the module scope. Returns a variable with
// index -1 if not found.
static VarDecl *resolveNonmodule(Compiler *compiler, const std::string &name) {
	// Look it up in the local scopes.
	VarDecl *variable = compiler->locals.Lookup(name);
	if (variable)
		return variable;

	// It's not a local, so guess that it's an upvalue.
	return findUpvalue(compiler, name);
}

// Finishes [compiler], which is compiling a function, method, or chunk of top
// level code. If there is a parent compiler, then this returns a expression creating
// a new closure of this function over the current variables.
static IRExpr *endCompiler(Compiler *compiler, std::string debugName) {
	// If we hit an error, don't finish the function since it's borked anyway.
	if (compiler->parser->hasError) {
		compiler->parser->context->compiler = compiler->parent;
		return NULL;
	}

	// wrenFunctionBindName(compiler->parser->context, compiler->fn, debugName, debugNameLength);

	// In the function that contains this one, load the resulting function object.
	ExprClosure *closure = nullptr;
	if (compiler->parent != NULL) {
		closure = compiler->New<ExprClosure>();
		closure->func = compiler->fn;
	}

	// Pop this compiler off the stack.
	compiler->parser->context->compiler = compiler->parent;

#if WREN_DEBUG_DUMP_COMPILED_CODE
	wrenDumpCode(compiler->parser->vm, compiler->fn);
#endif

	return closure;
}

// Grammar ---------------------------------------------------------------------

typedef enum {
	PREC_NONE,
	PREC_LOWEST,
	PREC_ASSIGNMENT,    // =
	PREC_CONDITIONAL,   // ?:
	PREC_LOGICAL_OR,    // ||
	PREC_LOGICAL_AND,   // &&
	PREC_EQUALITY,      // == !=
	PREC_IS,            // is
	PREC_COMPARISON,    // < > <= >=
	PREC_BITWISE_OR,    // |
	PREC_BITWISE_XOR,   // ^
	PREC_BITWISE_AND,   // &
	PREC_BITWISE_SHIFT, // << >>
	PREC_RANGE,         // .. ...
	PREC_TERM,          // + -
	PREC_FACTOR,        // * / %
	PREC_UNARY,         // unary - ! ~
	PREC_CALL,          // . () []
	PREC_PRIMARY
} Precedence;

typedef IRExpr *(*GrammarFn)(Compiler *, bool canAssign);
typedef IRExpr *(*GrammarInfixFn)(Compiler *, IRExpr *lhs, bool canAssign);

typedef void (*SignatureFn)(Compiler *compiler, Signature *signature);

typedef struct {
	GrammarFn prefix;
	GrammarInfixFn infix;
	SignatureFn method;
	Precedence precedence;
	const char *name;
} GrammarRule;

// Forward declarations since the grammar is recursive.
static GrammarRule *getRule(TokenType type);
static IRExpr *expression(Compiler *compiler);
static IRStmt *statement(Compiler *compiler);
static IRNode *definition(Compiler *compiler);
static IRExpr *parsePrecedence(Compiler *compiler, Precedence precedence);

// Parses a block body, after the initial "{" has been consumed.
//
// If the block was an expression body, an IRExpr representing it is returned.
// If the block is not an expression, then an IRNode is returned.
static IRNode *finishBlock(Compiler *compiler) {
	// Empty blocks do nothing.
	if (match(compiler, TOKEN_RIGHT_BRACE))
		return compiler->New<StmtBlock>();

	// If there's no line after the "{", it's a single-expression body.
	if (!matchLine(compiler)) {
		IRExpr *expr = expression(compiler);
		consume(compiler, TOKEN_RIGHT_BRACE, "Expect '}' at end of block.");
		return expr;
	}

	StmtBlock *block = compiler->New<StmtBlock>();

	// Empty blocks (with just a newline inside) do nothing.
	if (match(compiler, TOKEN_RIGHT_BRACE))
		return block;

	// Compile the definition list.
	do {
		// Grab the next token, so we can print an error at the correct token if this definition isn't allowed
		Token definitionToken = compiler->parser->next;

		IRNode *node = definition(compiler);
		consumeLine(compiler, "Expect newline after statement.");

		if (!node)
			continue;

		IRClass *cls = dynamic_cast<IRClass *>(node);
		if (cls) {
			// Note we don't have to handle system classes here, as wren_core declares all it's classes
			// at the top level. We don't have an assertion for it in case someone does - the class
			// declaration code will produce an error, but we'll just handle it normally rather than
			// aborting, to reveal any other errors in the file.

			compiler->parser->targetModule->AddNode(cls);
			cls->dynamicallyDefined = true;

			// Load the previous token, both for debug info when defining the class, and for the
			// error message if we can't allocate the local.
			compiler->parser->previous = definitionToken;

			// Declare a local variable to store this class instance
			LocalVariable *classOutput = addLocal(compiler, cls->info->name);
			if (!classOutput) {
				error(compiler, "The local output variable for class '%s' was already defined\n",
				    cls->info->name.c_str());
				continue;
			}

			// First, pass the class through an SSA variable though, since the SSA pass
			// will be unhappy if we put a local into outputVariable.
			SSAVariable *ssaOutput = addTemporary(compiler, classOutput->name + "_clssa"); // clssa for CLass SSA

			// Perform the actual class definition
			StmtDefineClass *classDef = compiler->New<StmtDefineClass>();
			classDef->targetClass = cls;
			classDef->outputVariable = ssaOutput;
			block->Add(classDef);

			// Copy the SSA into the local.
			compiler->AddNew<StmtAssign>(block, classOutput, compiler->New<ExprLoad>(ssaOutput));

			continue;
		}

		// Don't support classes declared in blocks etc
		IRStmt *stmt = dynamic_cast<IRStmt *>(node);
		if (!stmt) {
			compiler->parser->previous = definitionToken;
			error(compiler, "Only statements are supported inside blocks");
			continue;
		}
		block->Add(stmt);
	} while (peek(compiler) != TOKEN_RIGHT_BRACE && peek(compiler) != TOKEN_EOF);

	consume(compiler, TOKEN_RIGHT_BRACE, "Expect '}' at end of block.");
	return block;
}

// Parses a method or function body, after the initial "{" has been consumed.
//
// If [Compiler->isInitializer] is `true`, this is the body of a constructor
// initializer. In that case, this adds the code to ensure it returns `this`.
static StmtBlock *finishBody(Compiler *compiler, bool isMethod) {
	// Declare the method receiver for methods, so it can be bound by upvalues like any other
	// local variable. It's not used when the user types 'this' though, since that has a
	// special IR node for it.
	IRStmt *setupThis = nullptr;
	LocalVariable *receiverVar = nullptr;
	if (isMethod) {
		receiverVar = addLocal(compiler, "this");
		ASSERT(receiverVar != nullptr, "Couldn't create 'this' variable");

		// Create the statement now, so it has debug information from this area.
		setupThis = compiler->New<StmtAssign>(receiverVar, compiler->New<ExprLoadReceiver>());
	}

	IRNode *body = finishBlock(compiler);

	IRExpr *expr = dynamic_cast<IRExpr *>(body);
	IRStmt *stmt = dynamic_cast<IRStmt *>(body);
	bool isExpressionBody = expr != nullptr;

	ASSERT(expr || stmt, "The contents of a block must either be a statement or an expression");
	ASSERT(compiler->locals.GetTopFrame() == 0, "Cannot finishBody of a non-root scope");

	IRExpr *returnValue = nullptr;
	StmtBlock *block = compiler->New<StmtBlock>();

	// Add the root-level StmtBeginUpvalues node, which is required if any upvalues are declared in the
	// body of this block.
	block->Add(compiler->fn->rootBeginUpvalues);

	// Only initialise the 'this' variable if it's used as an upvalue. The user can't access it - writing
	// 'this' results in a ExprLoadReceiver - so we can leave it out to avoid cluttering up the IR.
	if (isMethod && !receiverVar->upvalues.empty()) {
		block->Add(setupThis);
	}

	// Initialise the local variable version of the arguments. The actual arguments
	// come in as SSA variables, so load them into the locals.
	for (int i = 0; i < (int)compiler->parameterLocals.size(); i++) {
		LocalVariable *local = compiler->parameterLocals.at(i);
		SSAVariable *ssa = compiler->fn->parameters.at(i);
		compiler->AddNew<StmtAssign>(block, local, compiler->New<ExprLoad>(ssa));
	}

	if (compiler->isInitializer) {
		// If the initializer body evaluates to a value, discard it.
		if (expr) {
			stmt = compiler->New<StmtEvalAndIgnore>(expr);
		}
		block->Add(stmt);

		// The receiver is always stored in the first local slot.
		returnValue = compiler->New<ExprLoadReceiver>();
	} else if (!isExpressionBody) {
		// Implicitly return null in statement bodies.
		block->Add(stmt);
		returnValue = null(compiler);
	} else {
		returnValue = expr;
	}

	// Free everything on the stack, making sure we evaluate the expression first so there aren't any
	// last-minute uses of the variables after discarding them.
	SSAVariable *tempReturn = addTemporary(compiler, "return_expr_temp");
	compiler->AddNew<StmtAssign>(block, tempReturn, returnValue);
	block->Add(discardLocals(compiler, compiler->locals.GetFramesSince(0)));

	block->Add(compiler->New<StmtReturn>(compiler->New<ExprLoad>(tempReturn)));
	return block;
}

// The VM can only handle a certain number of parameters, so check that we
// haven't exceeded that and give a usable error.
static void validateNumParameters(Compiler *compiler, int numArgs) {
	if (numArgs == MAX_PARAMETERS + 1) {
		// Only show an error at exactly max + 1 so that we can keep parsing the
		// parameters and minimize cascaded errors.
		error(compiler, "Methods cannot have more than %d parameters.", MAX_PARAMETERS);
	}
}

static void addParameterLocal(Compiler *compiler, LocalVariable *local) {
	compiler->parameterLocals.push_back(local);

	// Make an SSA variable to accept the parameter.
	SSAVariable *ssa = compiler->New<SSAVariable>();
	ssa->name = local->name + "_pssa"; // For Parameter SSA
	compiler->fn->parameters.push_back(ssa);
	compiler->fn->ssaVars.push_back(ssa);
}

// Parses the rest of a comma-separated parameter list after the opening
// delimeter. Updates `arity` in [signature] with the number of parameters.
static void finishParameterList(Compiler *compiler, Signature *signature) {
	do {
		ignoreNewlines(compiler);
		validateNumParameters(compiler, ++signature->arity);

		// Define a local variable in the method for the parameter.
		VarDecl *var = declareNamedVariable(compiler);

		// Mark it as function argument, to indicate this variable actually comes
		// from the parameter list (in regular Wren this is implicit from the stack position).
		LocalVariable *local = dynamic_cast<LocalVariable *>(var);
		if (!local) {
			std::string name = var->Name();
			error(compiler, "Parameter '%s' is not a local variable!", name.c_str());
			continue;
		}

		addParameterLocal(compiler, local);
	} while (match(compiler, TOKEN_COMMA));
}

// Returns [numParams] "_" surrounded by [leftBracket] and [rightBracket].
static std::string signatureParameterList(int numParams, char leftBracket, char rightBracket) {
	std::string name;
	name += leftBracket;

	// This function may be called with too many parameters. When that happens,
	// a compile error has already been reported, but we need to make sure we
	// don't overflow the string too, hence the MAX_PARAMETERS check.
	for (int i = 0; i < numParams && i < MAX_PARAMETERS; i++) {
		if (i > 0)
			name += ',';
		name += '_';
	}
	name += rightBracket;
	return name;
}

// Fills [name] with the stringified version of [signature] and updates
// [length] to the resulting length.
std::string Signature::ToString() const {
	std::string str;

	// Build the full name from the signature.
	str = name;

	switch (type) {
	case SIG_METHOD:
		str += signatureParameterList(arity, '(', ')');
		break;

	case SIG_GETTER:
		// The signature is just the str.
		break;

	case SIG_SETTER:
		str += '=';
		str += signatureParameterList(1, '(', ')');
		break;

	case SIG_SUBSCRIPT:
		str += signatureParameterList(arity, '[', ']');
		break;

	case SIG_SUBSCRIPT_SETTER:
		str += signatureParameterList(arity - 1, '[', ']');
		str += '=';
		str += signatureParameterList(1, '(', ')');
		break;

	case SIG_INITIALIZER:
		str = "init " + str;
		str += signatureParameterList(arity, '(', ')');
		break;
	}

	return str;
}

// Gets the symbol for a method with [signature].
static Signature *normaliseSignature(Compiler *compiler, const Signature &signature) {
	// Only use one signature object, so if the pointers are different it means a different thing
	return compiler->parser->context->GetSignature(signature);
}

// Returns a signature with [type] whose name is from the last consumed token.
static Signature signatureFromToken(Compiler *compiler, SignatureType type) {
	Signature signature;

	// Get the token for the method name.
	Token *token = &compiler->parser->previous;
	signature.name = token->contents;
	signature.type = type;
	signature.arity = 0;

	if (signature.name.size() > MAX_METHOD_NAME) {
		error(compiler, "Method names cannot be longer than %d characters.", MAX_METHOD_NAME);
	}

	return signature;
}

// Parses a comma-separated list of arguments. Modifies [signature] to include
// the arity of the argument list.
static std::vector<IRExpr *> finishArgumentList(Compiler *compiler, Signature *signature) {
	std::vector<IRExpr *> args;

	do {
		ignoreNewlines(compiler);
		validateNumParameters(compiler, ++signature->arity);
		IRExpr *expr = expression(compiler);
		ASSERT(expr, "expression() returned nullptr");
		args.push_back(expr);
	} while (match(compiler, TOKEN_COMMA));

	// Allow a newline before the closing delimiter.
	ignoreNewlines(compiler);

	return args;
}

// Compiles a method call with [args] for a method with [signature] expressed as a string, and calls it on [receiver].
// If [toAdd] is non-nullptr, then the method is added to the block and nullptr is returned (to avoid accidentally using
// the expression twice). This is merely a convenience to avoid having to manually add the method to a block.
static ExprFuncCall *callMethod(Compiler *compiler, std::string signature, IRExpr *receiver, std::vector<IRExpr *> args,
    StmtBlock *toAdd = nullptr) {
	Signature *sig = normaliseSignature(compiler, Signature::Parse(signature));

	ASSERT((int)args.size() == sig->arity, "callMethod signature and args have different arities");

	ExprFuncCall *call = compiler->New<ExprFuncCall>();
	call->signature = sig;
	call->receiver = receiver;
	call->args = args;

	if (!toAdd)
		return call;

	toAdd->Add(compiler->New<StmtEvalAndIgnore>(call));
	return nullptr;
}

// Compiles an (optional) argument list for a method call with [methodSignature]
// and then calls it.
static IRExpr *methodCall(Compiler *compiler, IRFn *superCaller, Signature *signature, IRExpr *receiver) {
	// Make a new signature that contains the updated arity and type based on
	// the arguments we find.
	Signature called = {signature->name, SIG_GETTER, 0};

	std::vector<IRExpr *> args;

	// Create the call now, so it has the line number of it's start if the call spans multiple lines
	ExprFuncCall *call = compiler->New<ExprFuncCall>();
	call->receiver = receiver;
	call->superCaller = superCaller;

	// Parse the argument list, if any.
	if (match(compiler, TOKEN_LEFT_PAREN)) {
		called.type = SIG_METHOD;

		// Allow new line before an empty argument list
		ignoreNewlines(compiler);

		// Allow empty an argument list.
		if (peek(compiler) != TOKEN_RIGHT_PAREN) {
			args = finishArgumentList(compiler, &called);
		}
		consume(compiler, TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");
	}

	// Parse the block argument, if any.
	if (match(compiler, TOKEN_LEFT_BRACE)) {
		// Include the block argument in the arity.
		called.type = SIG_METHOD;
		called.arity++;

		Compiler fnCompiler;
		initCompiler(&fnCompiler, compiler->parser, compiler);
		std::string subDebugName = signature->name + "_" + std::to_string(compiler->parser->previous.line);
		fnCompiler.fn->debugName = compiler->fn->debugName + "::" + subDebugName;
		fnCompiler.fn->parent = compiler->fn;
		compiler->parser->targetModule->AddNode(fnCompiler.fn);
		compiler->fn->closures.push_back(fnCompiler.fn);

		// Make a dummy signature to track the arity.
		Signature fnSignature = {"", SIG_METHOD, 0};

		// Parse the parameter list, if any.
		if (match(compiler, TOKEN_PIPE)) {
			finishParameterList(&fnCompiler, &fnSignature);
			consume(compiler, TOKEN_PIPE, "Expect '|' after function parameters.");
		}

		fnCompiler.fn->body = finishBody(&fnCompiler, false);

		// Name the function based on the method its passed to.
		std::string name = signature->ToString() + " block argument";

		IRExpr *closure = endCompiler(&fnCompiler, name);
		args.push_back(closure);
	}

	// TODO: Allow Grace-style mixfix methods?

	// If this is a super() call for an initializer, make sure we got an actual
	// argument list.
	if (signature->type == SIG_INITIALIZER) {
		if (called.type != SIG_METHOD) {
			error(compiler, "A superclass constructor must have an argument list.");
		}

		called.type = SIG_INITIALIZER;
	}

	call->signature = normaliseSignature(compiler, called);
	call->args = std::move(args);
	return call;
}

// Compiles a call whose name is the previously consumed token. This includes
// getters, method calls with arguments, and setter calls.
static IRExpr *namedCall(Compiler *compiler, IRExpr *receiver, bool canAssign, IRFn *superCaller) {
	// Get the token for the method name.
	Signature signature = signatureFromToken(compiler, SIG_GETTER);

	if (canAssign && match(compiler, TOKEN_EQ)) {
		ignoreNewlines(compiler);

		// Build the setter signature.
		signature.type = SIG_SETTER;
		signature.arity = 1;

		// Compile the assigned value.
		IRExpr *value = expression(compiler);

		ExprFuncCall *call = compiler->New<ExprFuncCall>();
		call->receiver = receiver;
		call->signature = normaliseSignature(compiler, signature);
		call->superCaller = superCaller;
		call->args.push_back(value);
		return call;
	}

	IRExpr *call = methodCall(compiler, superCaller, &signature, receiver);
	allowLineBeforeDot(compiler);
	return call;
}

// Emits the code to load [variable] onto the stack.
static IRExpr *loadVariable(Compiler *compiler, VarDecl *variable) { return compiler->New<ExprLoad>(variable); }

// Loads the receiver of the currently enclosing method. Correctly handles
// functions defined inside methods.
static IRExpr *loadThis(Compiler *compiler) {
	// If we're in a closure nested inside a function, then we need to indicate to the backend
	// that we need to use the receiver as an upvalue.
	// Note that we're using parent->enclosingClass since we want to check if we're in a method body - see
	// the comment for enclosingClass about this counterintuitive behaviour.
	if (compiler->parent->enclosingClass == nullptr) {
		VarDecl *upvalue = findUpvalue(compiler, "this");
		ASSERT(upvalue != nullptr, "Failed to create a 'this' upvalue!");
		return loadVariable(compiler, upvalue);
	}

	return compiler->New<ExprLoadReceiver>();
}

// Pushes the value for a module-level variable implicitly imported from core.
static IRExpr *loadCoreVariable(Compiler *compiler, std::string name) { return compiler->New<ExprSystemVar>(name); }

// A parenthesized expression.
static IRExpr *grouping(Compiler *compiler, bool canAssign) {
	IRExpr *expr = expression(compiler);
	consume(compiler, TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
	return expr;
}

// A list literal.
static IRExpr *list(Compiler *compiler, bool canAssign) {
	// Make a block to put our initialising statements in, and wrap that into an expression
	SSAVariable *list = addTemporary(compiler, "list-builder");
	StmtBlock *block = compiler->New<StmtBlock>();
	ExprRunStatements *expr = compiler->New<ExprRunStatements>();
	expr->temporary = list;
	expr->statement = block;

	// Instantiate a new list.
	IRExpr *newListExpr = callMethod(compiler, "new()", loadCoreVariable(compiler, "List"), {});
	block->Add(compiler->New<StmtAssign>(list, newListExpr));

	// Compile the list elements. Each one compiles to a ".add()" call.
	do {
		ignoreNewlines(compiler);

		// Stop if we hit the end of the list.
		if (peek(compiler) == TOKEN_RIGHT_BRACKET)
			break;

		// The element.
		IRExpr *toAdd = expression(compiler);
		callMethod(compiler, "add(_)", loadVariable(compiler, list), {toAdd}, block);
	} while (match(compiler, TOKEN_COMMA));

	// Allow newlines before the closing ']'.
	ignoreNewlines(compiler);
	consume(compiler, TOKEN_RIGHT_BRACKET, "Expect ']' after list elements.");

	return expr;
}

// A map literal.
static IRExpr *map(Compiler *compiler, bool canAssign) {
	// Make a block to put our initialising statements in, and wrap that into an expression
	SSAVariable *map = addTemporary(compiler, "map-builder");
	StmtBlock *block = compiler->New<StmtBlock>();
	ExprRunStatements *expr = compiler->New<ExprRunStatements>();
	expr->temporary = map;
	expr->statement = block;

	// Instantiate a new map.
	IRExpr *newMap = callMethod(compiler, "new()", loadCoreVariable(compiler, "Map"), {});
	block->Add(compiler->New<StmtAssign>(map, newMap));

	// Compile the map elements. Each one is compiled to just invoke the
	// subscript setter on the map.
	do {
		ignoreNewlines(compiler);

		// Stop if we hit the end of the map.
		if (peek(compiler) == TOKEN_RIGHT_BRACE)
			break;

		// The key.
		IRExpr *key = parsePrecedence(compiler, PREC_UNARY);
		consume(compiler, TOKEN_COLON, "Expect ':' after map key.");
		ignoreNewlines(compiler);

		// The value.
		IRExpr *toAdd = expression(compiler);
		callMethod(compiler, "[_]=(_)", loadVariable(compiler, map), {key, toAdd}, block);
	} while (match(compiler, TOKEN_COMMA));

	// Allow newlines before the closing '}'.
	ignoreNewlines(compiler);
	consume(compiler, TOKEN_RIGHT_BRACE, "Expect '}' after map entries.");

	return expr;
}

// Unary operators like `-foo`.
static IRExpr *unaryOp(Compiler *compiler, bool canAssign) {
	GrammarRule *rule = getRule(compiler->parser->previous.type);

	ignoreNewlines(compiler);

	// Compile the argument.
	IRExpr *receiver = parsePrecedence(compiler, (Precedence)(PREC_UNARY + 1));

	// Call the operator method on the left-hand side.
	return callMethod(compiler, rule->name, receiver, {});
}

static IRExpr *boolean(Compiler *compiler, bool canAssign) {
	CcValue value = CcValue(compiler->parser->previous.type == TOKEN_TRUE);
	return compiler->New<ExprConst>(value);
}

// Walks the compiler chain to find the compiler for the nearest class
// enclosing this one. Returns NULL if not currently inside a class definition.
static Compiler *getEnclosingClassCompiler(Compiler *compiler) {
	while (compiler != NULL) {
		if (compiler->enclosingClass != NULL)
			return compiler;
		compiler = compiler->parent;
	}

	return NULL;
}

// Walks the compiler chain to find the nearest class enclosing this one.
// Returns NULL if not currently inside a class definition.
static ClassInfo *getEnclosingClass(Compiler *compiler) {
	compiler = getEnclosingClassCompiler(compiler);
	return compiler == NULL ? NULL : compiler->enclosingClass;
}

// Walks the compiler chain to find the nearest method enclosing this one.
// Returns NULL if not currently inside a method definition.
static IRFn *getEnclosingMethod(Compiler *compiler) {
	while (compiler != nullptr) {
		if (compiler->fn->enclosingClass != nullptr)
			return compiler->fn;
		compiler = compiler->parent;
	}
	return nullptr;
}

static IRExpr *field(Compiler *compiler, bool canAssign) {
	// Initialize it with a fake value so we can keep parsing and minimize the
	// number of cascaded errors.
	FieldVariable *field = nullptr;

	ClassInfo *enclosingClass = getEnclosingClass(compiler);

	if (enclosingClass == NULL) {
		error(compiler, "Cannot reference a field outside of a class definition.");
	} else if (enclosingClass->isForeign) {
		error(compiler, "Cannot define fields in a foreign class.");
	} else if (enclosingClass->inStatic) {
		error(compiler, "Cannot use an instance field in a static method.");
	} else {
		// Look up the field, or implicitly define it.
		field = enclosingClass->fields.Ensure(compiler->parser->previous.contents);

		if (field->Id() >= MAX_FIELDS) {
			error(compiler, "A class can only have %d fields.", MAX_FIELDS);
		}
	}

	// Note that we're using parent->enclosingClass since we want to check if we're in a method body - see
	// the comment for enclosingClass about this counterintuitive behaviour.
	VarDecl *thisVar;
	if (compiler->parent == nullptr) {
		// An error should have already been generated, avoid crashing here
		ASSERT(enclosingClass == nullptr, "Enclosing class should be null for root-level field access");
	} else if (compiler->parent->enclosingClass != nullptr) {
		// Methods don't need to specify the 'this' variable
		thisVar = nullptr;
	} else {
		// Closures, however, do.
		thisVar = findUpvalue(compiler, "this");
		ASSERT(thisVar != nullptr, "Failed to create a 'this' upvalue for field access!");
	}

	// If there's an "=" after a field name, it's an assignment.
	if (canAssign && match(compiler, TOKEN_EQ)) {
		// Compile the right-hand side.
		IRExpr *value = expression(compiler);

		// Assignments in Wren can be used as expressions themselves, so we have to return an expression
		ExprRunStatements *run = compiler->New<ExprRunStatements>();
		SSAVariable *temporary = addTemporary(compiler, "set-field-expr");
		StmtBlock *block = compiler->New<StmtBlock>();
		run->statement = block;
		run->temporary = temporary;

		block->Add(compiler->New<StmtAssign>(temporary, value));
		block->Add(compiler->New<StmtFieldAssign>(thisVar, field, loadVariable(compiler, temporary)));

		return run;
	}

	allowLineBeforeDot(compiler);

	return compiler->New<ExprFieldLoad>(thisVar, field);
}

// Compiles a read or assignment to [variable].
static IRExpr *bareName(Compiler *compiler, bool canAssign, VarDecl *variable) {
	// If there's an "=" after a bare name, it's a variable assignment.
	if (canAssign && match(compiler, TOKEN_EQ)) {
		// Compile the right-hand side.
		IRExpr *expr = expression(compiler);

		// This has to be an expression, so wrap it up like that
		ExprRunStatements *wrapper = compiler->New<ExprRunStatements>();
		StmtBlock *block = compiler->New<StmtBlock>();
		SSAVariable *tmp = addTemporary(compiler, "bareName-assign-" + variable->Name());
		wrapper->statement = block;
		wrapper->temporary = tmp;

		// Emit the assignments - first store to the temporary, then store that to the target variable
		block->Add(compiler->New<StmtAssign>(tmp, expr));
		block->Add(compiler->New<StmtAssign>(variable, loadVariable(compiler, tmp)));

		return wrapper;
	}

	IRExpr *expr = loadVariable(compiler, variable);
	allowLineBeforeDot(compiler);
	return expr;
}

static IRExpr *staticField(Compiler *compiler, bool canAssign) {
	Compiler *classCompiler = getEnclosingClassCompiler(compiler);
	if (classCompiler == NULL) {
		error(compiler, "Cannot use a static field outside of a class definition.");
		return null(compiler);
	}

	// Look up the name in the scope chain.
	Token *token = &compiler->parser->previous;

	ClassInfo *cls = classCompiler->enclosingClass;
	FieldVariable *fieldVar = cls->staticFields.Ensure(token->contents);

	// For now, we'll only handle classes that we know are only declared once.
	// This means we can just use a global variable to store the static field.
	if (!fieldVar->staticGlobal) {
		fieldVar->staticGlobal = std::make_unique<IRGlobalDecl>();
		fieldVar->staticGlobal->name = cls->name + "::" + token->contents;
	}

	return bareName(compiler, canAssign, fieldVar->staticGlobal.get());
}

// Compiles a variable name or method call with an implicit receiver.
static IRExpr *name(Compiler *compiler, bool canAssign) {
	// Look for the name in the scope chain up to the nearest enclosing method.
	Token *token = &compiler->parser->previous;

	VarDecl *variable = resolveNonmodule(compiler, token->contents);
	if (variable) {
		return bareName(compiler, canAssign, variable);
	}

	// TODO: The fact that we return above here if the variable is known and parse
	//  an optional argument list below if not means that the grammar is not
	//  context-free. A line of code in a method like "someName(foo)" is a parse
	//  error if "someName" is a defined variable in the surrounding scope and not
	//  if it isn't. Fix this. One option is to have "someName(foo)" always
	//  resolve to a self-call if there is an argument list, but that makes
	//  getters a little confusing.

	// If we're inside a method and the name is lowercase, treat it as a method
	// on this.
	if (wrenIsLocalName(token->contents) && getEnclosingClass(compiler) != NULL) {
		return namedCall(compiler, loadThis(compiler), canAssign, nullptr);
	}

	// Check if a system variable with the same name exists (eg, for Object).
	// In the core module, only do this for classes defined in C++ - the classes
	// defined in Wren need to use normal global variables so they can reference
	// each other.
	const auto *sysVarNames = &ExprSystemVar::SYSTEM_VAR_NAMES;
	if (compiler->parser->compilingInternal)
		sysVarNames = &ExprSystemVar::CPP_SYSTEM_VAR_NAMES;
	if (sysVarNames->contains(token->contents))
		return compiler->New<ExprSystemVar>(token->contents);

	// Otherwise, look for a module-level variable with the name.
	variable = compiler->parser->targetModule->FindVariable(token->contents);
	if (variable == nullptr) {
		// Implicitly define a module-level variable in the hopes that we get a real definition later.
		IRGlobalDecl *decl = compiler->parser->targetModule->AddVariable(token->contents);
		ASSERT(decl, "Could not find or create global variable");
		decl->undeclaredLineUsed = token->line;
		variable = decl;
	}

	return bareName(compiler, canAssign, variable);
}

static IRExpr *null(Compiler *compiler, bool canAssign) { return compiler->New<ExprConst>(CcValue::NULL_TYPE); }

// A number or string literal.
static IRExpr *literal(Compiler *compiler, bool canAssign = false) {
	return compiler->New<ExprConst>(compiler->parser->previous.value);
}

// A string literal that contains interpolated expressions.
//
// Interpolation is syntactic sugar for calling ".join()" on a list. So the
// string:
//
//     "a %(b + c) d"
//
// is compiled roughly like:
//
//     ["a ", b + c, " d"].join()
static IRExpr *stringInterpolation(Compiler *compiler, bool canAssign) {
	// Make a block to put our initialising statements in, and wrap that into an expression
	SSAVariable *parts = addTemporary(compiler, "string-interpolation-parts");
	SSAVariable *result = addTemporary(compiler, "string-interpolation-result");
	StmtBlock *block = compiler->New<StmtBlock>();
	ExprRunStatements *expr = compiler->New<ExprRunStatements>();
	expr->temporary = result;
	expr->statement = block;

	// Instantiate a new list.
	IRExpr *newList = callMethod(compiler, "new()", loadCoreVariable(compiler, "List"), {});
	block->Add(compiler->New<StmtAssign>(parts, newList));

	do {
		// The opening string part.
		IRExpr *lit = literal(compiler);
		callMethod(compiler, "add(_)", loadVariable(compiler, parts), {lit}, block);

		// The interpolated expression.
		ignoreNewlines(compiler);
		IRExpr *interpolatedExpr = expression(compiler);
		callMethod(compiler, "add(_)", loadVariable(compiler, parts), {interpolatedExpr}, block);

		ignoreNewlines(compiler);
	} while (match(compiler, TOKEN_INTERPOLATION));

	// The trailing string part.
	consume(compiler, TOKEN_STRING, "Expect end of string interpolation.");
	IRExpr *trailing = literal(compiler);
	callMethod(compiler, "add(_)", loadVariable(compiler, parts), {trailing}, block);

	// The list of interpolated parts. Join them all together and store them in the result variable, as that's set
	// as the output of the ExprRunStatements.
	IRExpr *joined = callMethod(compiler, "join()", loadVariable(compiler, parts), {});
	block->Add(compiler->New<StmtAssign>(result, joined));

	return expr;
}

static IRExpr *super_(Compiler *compiler, bool canAssign) {
	IRFn *enclosingMethod = getEnclosingMethod(compiler);
	Signature *enclosingSignature;
	IRExpr *thisExpr;
	if (enclosingMethod == nullptr) {
		error(compiler, "Cannot use 'super' outside of a method.");

		// Put a dummy value in, to let compilation continue.
		thisExpr = compiler->New<ExprConst>(CcValue::NULL_TYPE);
		enclosingSignature = compiler->parser->context->alloc.New<Signature>();
	} else {
		// loadThis crashes if we're not in a method or a closure declared with it, so only
		// call this if that's the case.
		thisExpr = loadThis(compiler);
		enclosingSignature = enclosingMethod->methodInfo->signature;
	}

	// TODO: Super operator calls.
	// TODO: There's no syntax for invoking a superclass constructor with a
	// different name from the enclosing one. Figure that out.

	// See if it's a named super call, or an unnamed one.
	if (match(compiler, TOKEN_DOT)) {
		// Compile the superclass call.
		consume(compiler, TOKEN_NAME, "Expect method name after 'super.'.");
		return namedCall(compiler, thisExpr, canAssign, enclosingMethod);
	}

	// No explicit name, so use the name of the enclosing method. Make sure we
	// check that enclosingMethod isn't NULL first. We've already reported the
	// error, but we don't want to crash here.
	return methodCall(compiler, enclosingMethod, enclosingSignature, thisExpr);
}

static IRExpr *this_(Compiler *compiler, bool canAssign) {
	if (getEnclosingClass(compiler) == NULL) {
		error(compiler, "Cannot use 'this' outside of a method.");
		return nullptr;
	}

	return loadThis(compiler);
}

// Subscript or "array indexing" operator like `foo[bar]`.
static IRExpr *subscript(Compiler *compiler, IRExpr *receiver, bool canAssign) {
	Signature signature = {"", SIG_SUBSCRIPT, 0};

	// Parse the argument list.
	std::vector<IRExpr *> args = finishArgumentList(compiler, &signature);
	consume(compiler, TOKEN_RIGHT_BRACKET, "Expect ']' after arguments.");

	allowLineBeforeDot(compiler);

	if (canAssign && match(compiler, TOKEN_EQ)) {
		signature.type = SIG_SUBSCRIPT_SETTER;

		// Compile the assigned value.
		validateNumParameters(compiler, ++signature.arity);
		IRExpr *value = expression(compiler);
		args.push_back(value);
	}

	ExprFuncCall *call = compiler->New<ExprFuncCall>();
	call->receiver = receiver;
	call->signature = normaliseSignature(compiler, signature);
	call->args = args;
	return call;
}

static IRExpr *call(Compiler *compiler, IRExpr *lhs, bool canAssign) {
	ignoreNewlines(compiler);
	consume(compiler, TOKEN_NAME, "Expect method name after '.'.");
	return namedCall(compiler, lhs, canAssign, nullptr);
}

static IRExpr *and_(Compiler *compiler, IRExpr *lhs, bool canAssign) {
	ignoreNewlines(compiler);

	// We have to run a series of statements including jumps to find
	// the value.
	StmtBlock *block = compiler->New<StmtBlock>();
	SSAVariable *tmp = addTemporary(compiler, "and-value-tmp");
	ExprRunStatements *wrapper = compiler->New<ExprRunStatements>();
	wrapper->statement = block;
	wrapper->temporary = tmp;

	// Since we can't reassign SSA variables, make a temporary non-SSA variable.
	LocalVariable *local = addMutableTemporary(compiler, "and-value-mut");

	// Put the LHS in the temporary
	compiler->AddNew<StmtAssign>(block, local, lhs);

	// Skip the right argument if the left is false.
	StmtJump *jump = compiler->AddNew<StmtJump>(block);
	jump->condition = loadVariable(compiler, local);
	jump->jumpOnFalse = true;
	IRExpr *rhs = parsePrecedence(compiler, PREC_LOGICAL_AND);
	compiler->AddNew<StmtAssign>(block, local, rhs);
	jump->target = compiler->AddNew<StmtLabel>(block, "and-target");

	compiler->AddNew<StmtAssign>(block, tmp, loadVariable(compiler, local));
	return wrapper;
}

static IRExpr *or_(Compiler *compiler, IRExpr *lhs, bool canAssign) {
	ignoreNewlines(compiler);

	// We have to run a series of statements including jumps to find
	// the value.
	StmtBlock *block = compiler->New<StmtBlock>();
	SSAVariable *tmp = addTemporary(compiler, "or-value-tmp");
	ExprRunStatements *wrapper = compiler->New<ExprRunStatements>();
	wrapper->statement = block;
	wrapper->temporary = tmp;

	// Since we can't reassign SSA variables, make a temporary non-SSA variable.
	LocalVariable *local = addMutableTemporary(compiler, "or-value-mut");

	// Put the LHS in the temporary
	compiler->AddNew<StmtAssign>(block, local, lhs);

	// Skip the right argument if the left is true.
	StmtJump *jump = compiler->AddNew<StmtJump>(block);
	jump->condition = loadVariable(compiler, local);
	IRExpr *rhs = parsePrecedence(compiler, PREC_LOGICAL_OR);
	compiler->AddNew<StmtAssign>(block, local, rhs);
	jump->target = compiler->AddNew<StmtLabel>(block, "or-target");

	compiler->AddNew<StmtAssign>(block, tmp, loadVariable(compiler, local));
	return wrapper;
}

static IRExpr *conditional(Compiler *compiler, IRExpr *condition, bool canAssign) {
	// Ignore newline after '?'.
	ignoreNewlines(compiler);

	// We have to run a series of statements including jumps to find
	// the value.
	StmtBlock *block = compiler->New<StmtBlock>();
	SSAVariable *tmp = addTemporary(compiler, "conditional-value-tmp");
	ExprRunStatements *wrapper = compiler->New<ExprRunStatements>();
	wrapper->statement = block;
	wrapper->temporary = tmp;

	// Since we can't reassign SSA variables, make a temporary non-SSA variable.
	LocalVariable *local = addMutableTemporary(compiler, "conditional-value-mut");

	// If the condition is false, jump to the 2nd term
	StmtJump *jumpToFalse = compiler->AddNew<StmtJump>(block);
	jumpToFalse->condition = condition;
	jumpToFalse->jumpOnFalse = true;

	// Compile the then branch.
	IRExpr *trueValue = parsePrecedence(compiler, PREC_CONDITIONAL);
	compiler->AddNew<StmtAssign>(block, local, trueValue);

	consume(compiler, TOKEN_COLON, "Expect ':' after then branch of conditional operator.");
	ignoreNewlines(compiler);

	// Jump over the else branch when the if branch is taken.
	StmtJump *trueToEnd = compiler->AddNew<StmtJump>(block);

	// Compile the else branch.
	jumpToFalse->target = compiler->AddNew<StmtLabel>(block, "condition-start-to-else");

	IRExpr *falseValue = parsePrecedence(compiler, PREC_ASSIGNMENT);
	compiler->AddNew<StmtAssign>(block, local, falseValue);

	// Patch the jump over the else.
	trueToEnd->target = compiler->AddNew<StmtLabel>(block, "condition-true-to-end");

	compiler->AddNew<StmtAssign>(block, tmp, loadVariable(compiler, local));

	return wrapper;
}

static IRExpr *infixOp(Compiler *compiler, IRExpr *receiver, bool canAssign) {
	GrammarRule *rule = getRule(compiler->parser->previous.type);

	// An infix operator cannot end an expression.
	ignoreNewlines(compiler);

	// Compile the right-hand side.
	IRExpr *rhs = parsePrecedence(compiler, (Precedence)(rule->precedence + 1));

	// Call the operator method on the left-hand side.
	Signature signature = {rule->name, SIG_METHOD, 1};

	ExprFuncCall *call = compiler->New<ExprFuncCall>();
	call->receiver = receiver;
	call->signature = normaliseSignature(compiler, signature);
	call->args.push_back(rhs);
	// TODO super handling - eg does "super * 5" do what you'd expect?
	return call;
}

// Compiles a method signature for an infix operator.
void infixSignature(Compiler *compiler, Signature *signature) {
	// Add the RHS parameter.
	signature->type = SIG_METHOD;
	signature->arity = 1;

	// Parse the parameter name.
	consume(compiler, TOKEN_LEFT_PAREN, "Expect '(' after operator name.");
	VarDecl *var = declareNamedVariable(compiler);
	consume(compiler, TOKEN_RIGHT_PAREN, "Expect ')' after parameter name.");

	// Mark it as function argument, to indicate this variable actually comes
	// from the parameter list (in regular Wren this is implicit from the stack position).
	LocalVariable *local = dynamic_cast<LocalVariable *>(var);
	if (!local) {
		std::string name = var->Name();
		error(compiler, "Parameter '%s' is not a local variable!", name.c_str());
	} else {
		addParameterLocal(compiler, local);
	}
}

// Compiles a method signature for an unary operator (i.e. "!").
void unarySignature(Compiler *compiler, Signature *signature) {
	// Do nothing. The name is already complete.
	signature->type = SIG_GETTER;
}

// Compiles a method signature for an operator that can either be unary or
// infix (i.e. "-").
void mixedSignature(Compiler *compiler, Signature *signature) {
	signature->type = SIG_GETTER;

	// If there is a parameter, it's an infix operator, otherwise it's unary.
	if (match(compiler, TOKEN_LEFT_PAREN)) {
		// Add the RHS parameter.
		signature->type = SIG_METHOD;
		signature->arity = 1;

		// Parse the parameter name.
		VarDecl *var = declareNamedVariable(compiler);
		consume(compiler, TOKEN_RIGHT_PAREN, "Expect ')' after parameter name.");

		LocalVariable *local = dynamic_cast<LocalVariable *>(var);
		if (!local) {
			std::string name = var->Name();
			error(compiler, "Parameter '%s' is not a local variable!", name.c_str());
		} else {
			addParameterLocal(compiler, local);
		}
	}
}

// Compiles an optional setter parameter in a method [signature].
//
// Returns `true` if it was a setter.
static bool maybeSetter(Compiler *compiler, Signature *signature) {
	// See if it's a setter.
	if (!match(compiler, TOKEN_EQ))
		return false;

	// It's a setter.
	if (signature->type == SIG_SUBSCRIPT) {
		signature->type = SIG_SUBSCRIPT_SETTER;
	} else {
		signature->type = SIG_SETTER;
	}

	// Parse the value parameter.
	consume(compiler, TOKEN_LEFT_PAREN, "Expect '(' after '='.");
	VarDecl *var = declareNamedVariable(compiler);
	consume(compiler, TOKEN_RIGHT_PAREN, "Expect ')' after parameter name.");

	LocalVariable *local = dynamic_cast<LocalVariable *>(var);
	if (!local) {
		std::string name = var->Name();
		error(compiler, "Parameter '%s' is not a local variable!", name.c_str());
	} else {
		addParameterLocal(compiler, local);
	}

	signature->arity++;

	return true;
}

// Compiles a method signature for a subscript operator.
void subscriptSignature(Compiler *compiler, Signature *signature) {
	signature->type = SIG_SUBSCRIPT;

	// The signature currently has "[" as its name since that was the token that
	// matched it. Clear that out.
	signature->name = "";

	// Parse the parameters inside the subscript.
	finishParameterList(compiler, signature);
	consume(compiler, TOKEN_RIGHT_BRACKET, "Expect ']' after parameters.");

	maybeSetter(compiler, signature);
}

// Parses an optional parenthesized parameter list. Updates `type` and `arity`
// in [signature] to match what was parsed.
static void parameterList(Compiler *compiler, Signature *signature) {
	// The parameter list is optional.
	if (!match(compiler, TOKEN_LEFT_PAREN))
		return;

	signature->type = SIG_METHOD;

	// Allow new line before an empty argument list
	ignoreNewlines(compiler);

	// Allow an empty parameter list.
	if (match(compiler, TOKEN_RIGHT_PAREN))
		return;

	finishParameterList(compiler, signature);
	consume(compiler, TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");
}

// Compiles a method signature for a named method or setter.
void namedSignature(Compiler *compiler, Signature *signature) {
	signature->type = SIG_GETTER;

	// If it's a setter, it can't also have a parameter list.
	if (maybeSetter(compiler, signature))
		return;

	// Regular named method with an optional parameter list.
	parameterList(compiler, signature);
}

// Compiles a method signature for a constructor.
void constructorSignature(Compiler *compiler, Signature *signature) {
	consume(compiler, TOKEN_NAME, "Expect constructor name after 'construct'.");

	// Capture the name.
	*signature = signatureFromToken(compiler, SIG_INITIALIZER);

	if (match(compiler, TOKEN_EQ)) {
		error(compiler, "A constructor cannot be a setter.");
	}

	if (!match(compiler, TOKEN_LEFT_PAREN)) {
		error(compiler, "A constructor cannot be a getter.");
		return;
	}

	// Allow an empty parameter list.
	if (match(compiler, TOKEN_RIGHT_PAREN))
		return;

	finishParameterList(compiler, signature);
	consume(compiler, TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");
}

// This table defines all of the parsing rules for the prefix and infix
// expressions in the grammar. Expressions are parsed using a Pratt parser.
//
// See: http://journal.stuffwithstuff.com/2011/03/19/pratt-parsers-expression-parsing-made-easy/
#define UNUSED                                                                                                         \
	{ NULL, NULL, NULL, PREC_NONE, NULL }
#define PREFIX(fn)                                                                                                     \
	{ fn, NULL, NULL, PREC_NONE, NULL }
#define INFIX(prec, fn)                                                                                                \
	{ NULL, fn, NULL, prec, NULL }
#define INFIX_OPERATOR(prec, name)                                                                                     \
	{ NULL, infixOp, infixSignature, prec, name }
#define PREFIX_OPERATOR(name)                                                                                          \
	{ unaryOp, NULL, unarySignature, PREC_NONE, name }
#define OPERATOR(name)                                                                                                 \
	{ unaryOp, infixOp, mixedSignature, PREC_TERM, name }

GrammarRule rules[] = {
    /* TOKEN_LEFT_PAREN    */ PREFIX(grouping),
    /* TOKEN_RIGHT_PAREN   */ UNUSED,
    /* TOKEN_LEFT_BRACKET  */ {list, subscript, subscriptSignature, PREC_CALL, NULL},
    /* TOKEN_RIGHT_BRACKET */ UNUSED,
    /* TOKEN_LEFT_BRACE    */ PREFIX(map),
    /* TOKEN_RIGHT_BRACE   */ UNUSED,
    /* TOKEN_COLON         */ UNUSED,
    /* TOKEN_DOT           */ INFIX(PREC_CALL, call),
    /* TOKEN_DOTDOT        */ INFIX_OPERATOR(PREC_RANGE, ".."),
    /* TOKEN_DOTDOTDOT     */ INFIX_OPERATOR(PREC_RANGE, "..."),
    /* TOKEN_COMMA         */ UNUSED,
    /* TOKEN_STAR          */ INFIX_OPERATOR(PREC_FACTOR, "*"),
    /* TOKEN_SLASH         */ INFIX_OPERATOR(PREC_FACTOR, "/"),
    /* TOKEN_PERCENT       */ INFIX_OPERATOR(PREC_FACTOR, "%"),
    /* TOKEN_HASH          */ UNUSED,
    /* TOKEN_PLUS          */ INFIX_OPERATOR(PREC_TERM, "+"),
    /* TOKEN_MINUS         */ OPERATOR("-"),
    /* TOKEN_LTLT          */ INFIX_OPERATOR(PREC_BITWISE_SHIFT, "<<"),
    /* TOKEN_GTGT          */ INFIX_OPERATOR(PREC_BITWISE_SHIFT, ">>"),
    /* TOKEN_PIPE          */ INFIX_OPERATOR(PREC_BITWISE_OR, "|"),
    /* TOKEN_PIPEPIPE      */ INFIX(PREC_LOGICAL_OR, or_),
    /* TOKEN_CARET         */ INFIX_OPERATOR(PREC_BITWISE_XOR, "^"),
    /* TOKEN_AMP           */ INFIX_OPERATOR(PREC_BITWISE_AND, "&"),
    /* TOKEN_AMPAMP        */ INFIX(PREC_LOGICAL_AND, and_),
    /* TOKEN_BANG          */ PREFIX_OPERATOR("!"),
    /* TOKEN_TILDE         */ PREFIX_OPERATOR("~"),
    /* TOKEN_QUESTION      */ INFIX(PREC_ASSIGNMENT, conditional),
    /* TOKEN_EQ            */ UNUSED,
    /* TOKEN_LT            */ INFIX_OPERATOR(PREC_COMPARISON, "<"),
    /* TOKEN_GT            */ INFIX_OPERATOR(PREC_COMPARISON, ">"),
    /* TOKEN_LTEQ          */ INFIX_OPERATOR(PREC_COMPARISON, "<="),
    /* TOKEN_GTEQ          */ INFIX_OPERATOR(PREC_COMPARISON, ">="),
    /* TOKEN_EQEQ          */ INFIX_OPERATOR(PREC_EQUALITY, "=="),
    /* TOKEN_BANGEQ        */ INFIX_OPERATOR(PREC_EQUALITY, "!="),
    /* TOKEN_BREAK         */ UNUSED,
    /* TOKEN_CONTINUE      */ UNUSED,
    /* TOKEN_CLASS         */ UNUSED,
    /* TOKEN_CONSTRUCT     */ {NULL, NULL, constructorSignature, PREC_NONE, NULL},
    /* TOKEN_ELSE          */ UNUSED,
    /* TOKEN_FALSE         */ PREFIX(boolean),
    /* TOKEN_FOR           */ UNUSED,
    /* TOKEN_FOREIGN       */ UNUSED,
    /* TOKEN_IF            */ UNUSED,
    /* TOKEN_IMPORT        */ UNUSED,
    /* TOKEN_AS            */ UNUSED,
    /* TOKEN_IN            */ UNUSED,
    /* TOKEN_IS            */ INFIX_OPERATOR(PREC_IS, "is"),
    /* TOKEN_NULL          */ PREFIX(null),
    /* TOKEN_RETURN        */ UNUSED,
    /* TOKEN_STATIC        */ UNUSED,
    /* TOKEN_SUPER         */ PREFIX(super_),
    /* TOKEN_THIS          */ PREFIX(this_),
    /* TOKEN_TRUE          */ PREFIX(boolean),
    /* TOKEN_VAR           */ UNUSED,
    /* TOKEN_WHILE         */ UNUSED,
    /* TOKEN_FIELD         */ PREFIX(field),
    /* TOKEN_STATIC_FIELD  */ PREFIX(staticField),
    /* TOKEN_NAME          */ {name, NULL, namedSignature, PREC_NONE, NULL},
    /* TOKEN_NUMBER        */ PREFIX(literal),
    /* TOKEN_STRING        */ PREFIX(literal),
    /* TOKEN_INTERPOLATION */ PREFIX(stringInterpolation),
    /* TOKEN_LINE          */ UNUSED,
    /* TOKEN_ERROR         */ UNUSED,
    /* TOKEN_EOF           */ UNUSED};

// Gets the [GrammarRule] associated with tokens of [type].
static GrammarRule *getRule(TokenType type) { return &rules[type]; }

// The main entrypoint for the top-down operator precedence parser.
IRExpr *parsePrecedence(Compiler *compiler, Precedence precedence) {
	nextToken(compiler->parser);
	GrammarFn prefix = rules[compiler->parser->previous.type].prefix;

	if (prefix == NULL) {
		error(compiler, "Expected expression.");

		// Return a dummy value to avoid problems
		return null(compiler);
	}

	// Track if the precendence of the surrounding expression is low enough to
	// allow an assignment inside this one. We can't compile an assignment like
	// a normal expression because it requires us to handle the LHS specially --
	// it needs to be an lvalue, not an rvalue. So, for each of the kinds of
	// expressions that are valid lvalues -- names, subscripts, fields, etc. --
	// we pass in whether or not it appears in a context loose enough to allow
	// "=". If so, it will parse the "=" itself and handle it appropriately.
	bool canAssign = precedence <= PREC_CONDITIONAL;
	IRExpr *expr = prefix(compiler, canAssign);

	while (precedence <= rules[compiler->parser->current.type].precedence) {
		nextToken(compiler->parser);
		GrammarInfixFn infix = rules[compiler->parser->previous.type].infix;
		expr = infix(compiler, expr, canAssign);
	}

	return expr;
}

// Parses an expression. Unlike statements, expressions leave a resulting value
// on the stack.
IRExpr *expression(Compiler *compiler) { return parsePrecedence(compiler, PREC_LOWEST); }

// Marks the beginning of a loop. Keeps track of the current instruction so we
// know what to loop back to at the end of the body.
static void startLoop(Compiler *compiler, Loop *loop, StmtLabel *start) {
	loop->enclosing = compiler->loop;
	loop->start = start;
	loop->contentFrameId = compiler->locals.GetTopFrame();
	compiler->loop = loop;
}

// If a condition is nullptr or evaluates to a falsy value, break out of
// the loop. Keeps track of the instruction so we can patch it
// later once we know where the end of the body is.
static StmtJump *exitLoop(Compiler *compiler, IRExpr *condition) {
	StmtJump *jmp = compiler->New<StmtJump>(nullptr, condition);
	jmp->jumpOnFalse = true;
	compiler->loop->exitJumps.push_back(jmp);
	return jmp;
}

// Compiles the body of the loop and tracks its extent so that contained "break"
// statements can be handled correctly.
static IRStmt *loopBody(Compiler *compiler) {
	StmtBlock *block = compiler->New<StmtBlock>();
	StmtLabel *label = compiler->New<StmtLabel>("loop-body");
	block->Add(label);
	block->Add(statement(compiler));
	compiler->loop->body = label;
	return block;
}

// Ends the current innermost loop. Patches up all jumps and breaks now that
// we know where the end of the loop is.
static IRStmt *endLoop(Compiler *compiler) {
	// We don't check for overflow here since the forward jump over the loop body
	// will report an error for the same problem.
	StmtBlock *block = compiler->New<StmtBlock>();
	StmtJump *returnToStart = compiler->New<StmtJump>(compiler->loop->start, nullptr);
	block->Add(returnToStart);
	returnToStart->looping = true;

	// Make a label that all the break jumps go to
	StmtLabel *breakToLabel = compiler->New<StmtLabel>("loop-end");
	block->Add(breakToLabel);

	// Find any break placeholder jumps, and set their jump target accordingly
	for (StmtJump *jump : compiler->loop->exitJumps) {
		jump->target = breakToLabel;
	}

	// Effectively pop the loop off our linked-list stack of loops
	compiler->loop = compiler->loop->enclosing;

	return block;
}

static IRStmt *forStatement(Compiler *compiler) {
	// A for statement like:
	//
	//     for (i in sequence.expression) {
	//       System.print(i)
	//     }
	//
	// Is compiled to bytecode almost as if the source looked like this:
	//
	//     {
	//       var seq_ = sequence.expression
	//       var iter_
	//       while (iter_ = seq_.iterate(iter_)) {
	//         var i = seq_.iteratorValue(iter_)
	//         System.print(i)
	//       }
	//     }
	//
	// It's not exactly this, because the synthetic variables `seq_` and `iter_`
	// actually get names that aren't valid Wren identfiers, but that's the basic
	// idea.
	//
	// The important parts are:
	// - The sequence expression is only evaluated once.
	// - The .iterate() method is used to advance the iterator and determine if
	//   it should exit the loop.
	// - The .iteratorValue() method is used to get the value at the current
	//   iterator position.

	StmtBlock *block = compiler->New<StmtBlock>();

	// Create a scope for the hidden local variables used for the iterator.
	block->Add(pushScope(compiler));

	consume(compiler, TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");
	consume(compiler, TOKEN_NAME, "Expect for loop variable name.");

	// Remember the name of the loop variable.
	std::string name = compiler->parser->previous.contents;

	consume(compiler, TOKEN_IN, "Expect 'in' after loop variable.");
	ignoreNewlines(compiler);

	// Evaluate the sequence expression and store it in a hidden local variable.
	// The space in the variable name ensures it won't collide with a user-defined
	// variable.
	IRExpr *seqValue = expression(compiler);

	// Try to avoid running code that'd fail with interpreted Wren.
	if (compiler->locals.VariableCount() + 2 > MAX_LOCALS) {
		error(compiler,
		    "Cannot declare more than %d variables in one scope. (Not enough space for for-loops internal variables)",
		    MAX_LOCALS);
		// Keep parsing to find other issues
	}
	LocalVariable *seqSlot = addLocal(compiler, "seq ");
	block->Add(compiler->New<StmtAssign>(seqSlot, seqValue));

	// Create another hidden local for the iterator object.
	LocalVariable *iterSlot = addLocal(compiler, "iter ");
	block->Add(compiler->New<StmtAssign>(iterSlot, null(compiler)));

	consume(compiler, TOKEN_RIGHT_PAREN, "Expect ')' after loop expression.");

	StmtLabel *startLabel = compiler->New<StmtLabel>("for-start");
	block->Add(startLabel);
	Loop loop;
	startLoop(compiler, &loop, startLabel);

	// Advance the iterator by calling the ".iterate" method on the sequence.
	ExprFuncCall *iterateCall =
	    callMethod(compiler, "iterate(_)", loadVariable(compiler, seqSlot), {loadVariable(compiler, iterSlot)});

	// Update and test the iterator.
	block->Add(compiler->New<StmtAssign>(iterSlot, iterateCall));
	block->Add(exitLoop(compiler, loadVariable(compiler, iterSlot)));

	// Get the current value in the sequence by calling ".iteratorValue".
	ExprFuncCall *valueCall =
	    callMethod(compiler, "iteratorValue(_)", loadVariable(compiler, seqSlot), {loadVariable(compiler, iterSlot)});

	// Bind the loop variable in its own scope. This ensures we get a fresh
	// variable each iteration so that closures for it don't all see the same one.
	block->Add(pushScope(compiler));
	LocalVariable *valueVar = addLocal(compiler, name);
	block->Add(compiler->New<StmtAssign>(valueVar, valueCall));

	block->Add(loopBody(compiler));

	// Loop variable.
	block->Add(popScope(compiler));

	block->Add(endLoop(compiler));

	// Hidden variables.
	block->Add(popScope(compiler));

	return block;
}

static IRStmt *ifStatement(Compiler *compiler) {
	StmtBlock *block = compiler->New<StmtBlock>();

	// Compile the condition.
	consume(compiler, TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
	IRExpr *condition = expression(compiler);
	consume(compiler, TOKEN_RIGHT_PAREN, "Expect ')' after if condition.");

	// Jump to the else branch if the condition is false.
	StmtJump *ifJump = compiler->AddNew<StmtJump>(block, nullptr, condition);
	ifJump->jumpOnFalse = true;

	// Compile the then branch.
	block->Add(statement(compiler));

	// Compile the else branch if there is one.
	if (match(compiler, TOKEN_ELSE)) {
		// Jump over the else branch when the if branch is taken.
		StmtJump *ifToEnd = compiler->AddNew<StmtJump>(block);

		StmtLabel *elseTarget = compiler->AddNew<StmtLabel>(block, "if-start-to-else");
		ifJump->target = elseTarget;

		block->Add(statement(compiler));

		// Patch the jump over the else.
		StmtLabel *end = compiler->AddNew<StmtLabel>(block, "if-then-to-end");
		ifToEnd->target = end;
	} else {
		StmtLabel *end = compiler->AddNew<StmtLabel>(block, "if-false-skip");
		ifJump->target = end;
	}

	return block;
}

static IRStmt *whileStatement(Compiler *compiler) {
	StmtBlock *block = compiler->New<StmtBlock>();
	StmtLabel *startLabel = compiler->AddNew<StmtLabel>(block, "while-start");

	Loop loop;
	startLoop(compiler, &loop, startLabel);

	// Compile the condition.
	consume(compiler, TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
	IRExpr *condition = expression(compiler);
	consume(compiler, TOKEN_RIGHT_PAREN, "Expect ')' after while condition.");

	StmtJump *jumpOut = compiler->AddNew<StmtJump>(block, nullptr, condition);
	jumpOut->jumpOnFalse = true;
	loop.exitJumps.push_back(jumpOut);

	block->Add(loopBody(compiler));

	block->Add(endLoop(compiler));

	return block;
}

// Compiles a simple statement. These can only appear at the top-level or
// within curly blocks. Simple statements exclude variable binding statements
// like "var" and "class" which are not allowed directly in places like the
// branches of an "if" statement.
//
// Unlike expressions, statements do not leave a value on the stack.
IRStmt *statement(Compiler *compiler) {
	if (match(compiler, TOKEN_BREAK)) {
		if (compiler->loop == NULL) {
			error(compiler, "Cannot use 'break' outside of a loop.");
			return nullptr;
		}

		// Since we will be jumping out of the scope, make sure any locals in it
		// are discarded first.
		StmtBlock *block = compiler->New<StmtBlock>();
		block->Add(discardLocals(compiler, compiler->locals.GetFramesSince(compiler->loop->contentFrameId)));

		// Add a placeholder jump. We don't yet know what the end-of-loop label is (it
		// won't have been created yet), so make it jump to nullptr and add it to the
		// list of jumps to be fixed up by endLoop.
		StmtJump *jump = compiler->AddNew<StmtJump>(block);
		compiler->loop->exitJumps.push_back(jump);
		return block;
	}
	if (match(compiler, TOKEN_CONTINUE)) {
		if (compiler->loop == NULL) {
			error(compiler, "Cannot use 'continue' outside of a loop.");
			return nullptr;
		}

		// Since we will be jumping out of the scope, make sure any locals in it
		// are discarded first.
		StmtBlock *block = compiler->New<StmtBlock>();
		block->Add(discardLocals(compiler, compiler->locals.GetFramesSince(compiler->loop->contentFrameId)));

		// emit a jump back to the top of the loop
		compiler->AddNew<StmtJump>(block, compiler->loop->start, nullptr);
		return block;
	}
	if (match(compiler, TOKEN_FOR)) {
		return forStatement(compiler);
	}
	if (match(compiler, TOKEN_IF)) {
		return ifStatement(compiler);
	}
	if (match(compiler, TOKEN_RETURN)) {
		StmtBlock *block = compiler->New<StmtBlock>();
		IRExpr *returnValue;

		// Compile the return value.
		if (peek(compiler) == TOKEN_LINE) {
			// If there's no expression after return, initializers should
			// return 'this' and regular methods should return null
			returnValue = compiler->isInitializer ? loadThis(compiler) : null(compiler);
		} else {
			if (compiler->isInitializer) {
				error(compiler, "A constructor cannot return a value.");
			}

			returnValue = expression(compiler);
		}

		// Free everything on the stack, making sure we evaluate the expression first so there aren't any
		// last-minute uses of the variables after discarding them.
		SSAVariable *tempReturn = addTemporary(compiler, "return_expr_temp");
		compiler->AddNew<StmtAssign>(block, tempReturn, returnValue);

		// Relocate any upvalues, and any other cleanup we have to do. Do this all the way down to the first
		// frame, since all the frames are obviously gone once we return.
		// The exception is if we're at the root scope, then everything is just a global variable anyway.
		if (compiler->scopeDepth != -1) {
			block->Add(discardLocals(compiler, compiler->locals.GetFramesSince(0)));
		}

		compiler->AddNew<StmtReturn>(block, loadVariable(compiler, tempReturn));

		return block;
	}
	if (match(compiler, TOKEN_WHILE)) {
		return whileStatement(compiler);
	}
	if (match(compiler, TOKEN_LEFT_BRACE)) {
		// Block statement.
		StmtBeginUpvalues *upvalueSetup = pushScope(compiler);

		IRNode *body = finishBlock(compiler);

		IRStmt *stmt = dynamic_cast<IRStmt *>(body);

		// Ignore nulls, they mean 'ignore this'
		if (body && !stmt) {
			// Block was an expression, so discard it.
			IRExpr *expr = dynamic_cast<IRExpr *>(body);
			ASSERT(expr, "finishBlock returned neither a statement nor an expression");
			stmt = compiler->New<StmtEvalAndIgnore>(expr);
		}

		// Cleanup for any variables on the stack
		// Note that if cleanup is false there's no upvalues, so it's safe
		// to ignore the upvaluesSetup node.
		IRStmt *cleanup = popScope(compiler);
		if (cleanup) {
			StmtBlock *block = compiler->New<StmtBlock>();
			block->Add(upvalueSetup);
			block->Add(stmt);
			block->Add(cleanup);
			stmt = block;
		}

		return stmt;
	}

	// Expression statement.
	IRExpr *expr = expression(compiler);
	return compiler->New<StmtEvalAndIgnore>(expr);
}

// Declares a method in the enclosing class with [signature].
//
// Reports an error if a method with that signature is already declared.
static MethodInfo *declareMethod(Compiler *compiler, Signature *signature, bool forceStatic) {
	// See if the class has already declared method with this signature.
	ClassInfo *classInfo = compiler->enclosingClass;
	bool isStatic = classInfo->inStatic || forceStatic;
	auto &methods = isStatic ? classInfo->staticMethods : classInfo->methods;

	// Check for duplicate methods
	auto existing = methods.find(signature);
	if (existing != methods.end()) {
		// Don't use isStatic b/c that's for constructors, which would be weird to include the static keyword for
		const char *staticPrefix = classInfo->inStatic ? "static " : "";
		std::string sigStr = signature->ToString();
		error(compiler, "Class %s already defines a %smethod '%s' at line %d.", compiler->enclosingClass->name.c_str(),
		    staticPrefix, sigStr.c_str(), existing->second->lineNum);
	}

	methods[signature] = std::make_unique<MethodInfo>();
	MethodInfo *method = methods.at(signature).get();

	method->lineNum = compiler->parser->previous.line;
	method->signature = signature;
	method->isStatic = isStatic;

	return method;
}

static CcValue consumeLiteral(Compiler *compiler, const char *message) {
	if (match(compiler, TOKEN_FALSE))
		return CcValue(false);
	if (match(compiler, TOKEN_TRUE))
		return CcValue(true);
	if (match(compiler, TOKEN_NUMBER))
		return compiler->parser->previous.value;
	if (match(compiler, TOKEN_STRING))
		return compiler->parser->previous.value;
	if (match(compiler, TOKEN_NAME))
		return compiler->parser->previous.value;

	error(compiler, message);
	nextToken(compiler->parser);
	return CcValue::NULL_TYPE;
}

static AttrContent valueToAttribute(bool runtimeAccess, CcValue value) {
	AttrContent content = {.runtimeAccess = runtimeAccess};

	switch (value.type) {
	case CcValue::NULL_TYPE:
		content.value = NULL_VAL;
		break;
	case CcValue::STRING:
		content.str = value.s;
		break;
	case CcValue::BOOL:
		content.boolean = value.b;
		break;
	case CcValue::INT:
	case CcValue::NUM:
		content.value = encode_number(value.n);
		break;
	default:
		fmt::print(stderr, "Invalid attribute value type {}\n", value.type);
		abort();
		break;
	}

	return content;
}

static std::unique_ptr<AttributePack> matchAttribute(Compiler *compiler) {
	std::unique_ptr<AttributePack> pack;

	while (match(compiler, TOKEN_HASH)) {
		if (!pack) {
			pack = std::make_unique<AttributePack>();
		}

		bool runtimeAccess = match(compiler, TOKEN_BANG);
		if (match(compiler, TOKEN_NAME)) {
			std::string group = compiler->parser->previous.value.CheckString();
			TokenType ahead = peek(compiler);
			if (ahead == TOKEN_EQ || ahead == TOKEN_LINE) {
				// This isn't a group - what we thought was the group is the name
				AttrKey key = {.name = group};

				CcValue value;
				if (match(compiler, TOKEN_EQ)) {
					value = consumeLiteral(compiler,
					    "Expect a Bool, Num, String or Identifier literal for an attribute value.");
				}
				pack->attributes[key].push_back(valueToAttribute(runtimeAccess, value));
			} else if (match(compiler, TOKEN_LEFT_PAREN)) {
				ignoreNewlines(compiler);
				if (match(compiler, TOKEN_RIGHT_PAREN)) {
					error(compiler, "Expected attributes in group, group cannot be empty.");
				} else {
					while (peek(compiler) != TOKEN_RIGHT_PAREN) {
						consume(compiler, TOKEN_NAME, "Expect name for attribute key.");
						std::string name = compiler->parser->previous.value.CheckString();
						AttrKey key = {.group = group, .name = name};
						CcValue value;
						if (match(compiler, TOKEN_EQ)) {
							value = consumeLiteral(compiler,
							    "Expect a Bool, Num, String or Identifier literal for an attribute value.");
						}
						pack->attributes[key].push_back(valueToAttribute(runtimeAccess, value));

						ignoreNewlines(compiler);
						if (!match(compiler, TOKEN_COMMA))
							break;
						ignoreNewlines(compiler);
					}

					ignoreNewlines(compiler);
					consume(compiler, TOKEN_RIGHT_PAREN, "Expected ')' after grouped attributes.");
				}
			} else {
				error(compiler, "Expect an equal, newline or grouping after an attribute key.");
			}
		} else {
			error(compiler, "Expect an attribute definition after #.");
		}

		consumeLine(compiler, "Expect newline after attribute.");
	}

	return pack;
}

// Compiles a method definition inside a class body.
//
// Returns `true` if it compiled successfully, or `false` if the method couldn't
// be parsed.
static bool method(Compiler *compiler, IRClass *classNode) {
	// Parse any attributes before the method and store them
	std::unique_ptr<AttributePack> attributes = matchAttribute(compiler);

	// TODO: What about foreign constructors?
	bool isForeign = match(compiler, TOKEN_FOREIGN);
	bool isStatic = match(compiler, TOKEN_STATIC);
	compiler->enclosingClass->inStatic = isStatic;

	SignatureFn signatureFn = rules[compiler->parser->current.type].method;
	nextToken(compiler->parser);

	if (signatureFn == NULL) {
		error(compiler, "Expect method definition.");
		return false;
	}

	// Build the method signature. For now don't normalise it, since signatureFn might change it anyway.
	Signature tmpSignature = signatureFromToken(compiler, SIG_GETTER);
	compiler->enclosingClass->signature = &tmpSignature;

	Compiler methodCompiler;
	initCompiler(&methodCompiler, compiler->parser, compiler);

	// Compile the method signature. This might change the signature, so we have to normalise it afterwards.
	signatureFn(&methodCompiler, &tmpSignature);
	Signature *signature = normaliseSignature(compiler, tmpSignature);
	compiler->enclosingClass->signature = signature;
	methodCompiler.fn->debugName = classNode->info->name + "::" + signature->ToString();

	methodCompiler.isInitializer = signature->type == SIG_INITIALIZER;

	if (isStatic && signature->type == SIG_INITIALIZER) {
		error(compiler, "A constructor cannot be static.");
	}

	// Check for duplicate methods. Doesn't matter that it's already been
	// defined, error will discard bytecode anyway.
	// Check if the method table already contains this symbol
	MethodInfo *method = declareMethod(compiler, signature, false);
	method->isForeign = isForeign;
	method->attributes = std::move(attributes);

	if (isForeign) {
		// We don't need the function we started compiling in the parameter list
		// any more. This is normally done by endCompiler, but we don't need to call that.
		methodCompiler.parser->context->compiler = methodCompiler.parent;
	} else {
		consume(compiler, TOKEN_LEFT_BRACE, "Expect '{' to begin method body.");
		method->fn = methodCompiler.fn;
		method->fn->enclosingClass = classNode;
		method->fn->methodInfo = method;

		// Set up enclosingClass and methodInfo first, so we can use them while parsing the body
		methodCompiler.fn->body = finishBody(&methodCompiler, true);

		endCompiler(&methodCompiler, signature->ToString());
		compiler->parser->targetModule->AddNode(method->fn);
	}

	if (signature->type == SIG_INITIALIZER) {
		// Also define a matching constructor method on the metaclass.
		// Constructors have two parts, kinda like C++'s destructors: one initialises the
		// memory and runs the user's code, and that's of type SIG_INITIALISER.
		// There's a regular static method with (aside from being SIG_METHOD) the same signature,
		// which first allocates the memory to store the object then calls the initialiser.
		Signature allocSignatureValue = *signature;
		allocSignatureValue.type = SIG_METHOD;
		MethodInfo *alloc = declareMethod(compiler, normaliseSignature(compiler, allocSignatureValue), true);

		alloc->isForeign = false; // For foreign classes, the allocate memory node handles the FFI stuff.

		IRFn *fn = compiler->New<IRFn>();
		compiler->parser->targetModule->AddNode(fn);
		fn->enclosingClass = classNode;
		fn->methodInfo = alloc;
		fn->debugName = method->fn->debugName + "::alloc";
		alloc->fn = fn;

		std::vector<SSAVariable *> args;
		for (SSAVariable *initArg : method->fn->parameters) {
			SSAVariable *arg = compiler->New<SSAVariable>();
			arg->name = initArg->name;
			fn->ssaVars.push_back(arg);
			fn->parameters.push_back(arg);
			args.push_back(arg);
		}

		StmtBlock *block = compiler->New<StmtBlock>();
		fn->body = block;

		LocalVariable *objLocal = compiler->New<LocalVariable>();
		objLocal->name = "alloc_temp_special";
		fn->locals.push_back(objLocal);

		// First, allocate the memory of the new instance
		ExprAllocateInstanceMemory *allocExpr = compiler->New<ExprAllocateInstanceMemory>();
		allocExpr->target = classNode;

		if (classNode->info->isForeign) {
			// Foreign classes get all the parameters passed to the native allocation method.
			allocExpr->foreignParameters = args;
		}

		block->Add(compiler->New<StmtAssign>(objLocal, allocExpr));

		// Now run the initialiser
		ExprFuncCall *call = compiler->New<ExprFuncCall>();
		call->signature = signature;
		call->receiver = compiler->New<ExprLoad>(objLocal);
		for (SSAVariable *arg : args) {
			call->args.push_back(loadVariable(compiler, arg));
		}
		block->Add(compiler->New<StmtEvalAndIgnore>(call));

		// Finally, return it
		block->Add(compiler->New<StmtReturn>(compiler->New<ExprLoad>(objLocal)));
	}

	return true;
}

// Compiles a class definition. Assumes the "class" token has already been
// consumed (along with a possibly preceding "foreign" token).
static IRNode *classDefinition(Compiler *compiler, bool isForeign, std::unique_ptr<AttributePack> attributes) {
	IRClass *classNode = compiler->New<IRClass>();
	classNode->info = std::make_unique<ClassInfo>();

	// Find the class's name
	consume(compiler, TOKEN_NAME, "Expect class name.");
	std::string className = compiler->parser->previous.contents;

	// Limit it's length to match Wren
	if (className.size() > MAX_VARIABLE_NAME) {
		error(compiler, "Class name cannot be longer than %d characters.", MAX_VARIABLE_NAME);
	}

	// Load the superclass (if there is one).
	IRExpr *parentTypeExpr;
	if (match(compiler, TOKEN_IS)) {
		parentTypeExpr = parsePrecedence(compiler, PREC_CALL);
	} else {
		// Implicitly inherit from Object.
		parentTypeExpr = nullptr;
	}

	// Stop users here if they're trying to extend a core C++ type
	// Unsurprisingly, we make an exception for wren_core
	ExprSystemVar *systemParent = dynamic_cast<ExprSystemVar *>(parentTypeExpr);
	if (systemParent && ExprSystemVar::CPP_SYSTEM_VAR_NAMES.contains(systemParent->name) &&
	    systemParent->name != "Object" && !compiler->parser->compilingInternal) {

		error(compiler, "Class %s cannot extend from system type %s", className.c_str(), systemParent->name.c_str());
	}

	ClassInfo &classInfo = *classNode->info;
	classInfo.isForeign = isForeign;
	classInfo.name = className;
	classInfo.parentClass = parentTypeExpr;
	classInfo.attributes = std::move(attributes);

	if (classInfo.IsSystemClass() && !compiler->parser->compilingInternal) {
		error(compiler, "Cannot compile class which shares a name with system class '%s'", className.c_str());
	}

	// Set up symbol buffers to track duplicate static and instance methods.
	compiler->enclosingClass = classNode->info.get();

	// Compile the method definitions.
	consume(compiler, TOKEN_LEFT_BRACE, "Expect '{' after class declaration.");
	matchLine(compiler);

	while (!match(compiler, TOKEN_RIGHT_BRACE)) {
		if (!method(compiler, classNode))
			break;

		// Don't require a newline after the last definition.
		if (match(compiler, TOKEN_RIGHT_BRACE))
			break;

		consumeLine(compiler, "Expect newline after definition in class.");
	}

	compiler->enclosingClass = NULL;

	classInfo.inStatic = false;
	classInfo.signature = nullptr;

	return classNode;
}

// Compiles an "import" statement.
//
// An import compiles to a series of instructions. Given:
//
//     import "foo" for Bar, Baz
//
// We compile a single IMPORT_MODULE "foo" instruction to load the module
// itself. When that finishes executing the imported module, it leaves the
// Module in vm->lastModule. Then, for Bar and Baz, we:
//
// * Declare a variable in the current scope with that name.
// * Emit an IMPORT_VARIABLE instruction to load the variable's value from the
//   other module.
// * Compile the code to store that value in the variable in this scope.
static IRNode *importStatement(Compiler *compiler) {
	ignoreNewlines(compiler);
	consume(compiler, TOKEN_STRING, "Expect a string after 'import'.");
	std::string moduleName = compiler->parser->previous.value.CheckString();

	// Add a node to say that we need the module. This is kinda just put anywhere inside the
	// file, with no concerns for ordering or matching up with if statements or anything like that.
	IRImport *importNode = compiler->New<IRImport>();
	importNode->moduleName = moduleName;
	compiler->parser->targetModule->AddNode(importNode);

	// At this point in the execution, require that the module is loaded if it wasn't already
	// This makes stuff like requiring modules inside if statements work properly. If you have
	// a debug module that throws an error when imported in release builds and all the imports
	// of it check if you're in release mode or not, then this is what makes that work properly.
	StmtLoadModule *load = compiler->New<StmtLoadModule>();
	load->importNode = importNode;

	// The for clause is optional.
	if (!match(compiler, TOKEN_FOR))
		return load;

	// Compile the comma-separated list of variables to import.
	do {
		ignoreNewlines(compiler);

		consume(compiler, TOKEN_NAME, "Expect variable name.");

		// We need to hold onto the source variable,
		// in order to reference it in the import later
		Token sourceVariableToken = compiler->parser->previous;

		// Store the symbol we care about for the variable
		VarDecl *slot;
		if (match(compiler, TOKEN_AS)) {
			// import "module" for Source as Dest
			// Use 'Dest' as the name by declaring a new variable for it.
			// This parses a name after the 'as' and defines it.
			slot = declareNamedVariable(compiler);
		} else {
			// import "module" for Source
			// Uses 'Source' as the name directly
			slot = declareVariable(compiler, &sourceVariableToken);
		}

		// Prevent these locals from being converted to SSA form. It's really
		// horrible, but there's not really any convenient way that makes
		// sense in the IR - where would the StmtAssign go?
		LocalVariable *local = dynamic_cast<LocalVariable *>(slot);
		if (local) {
			local->disableSSA = true;
		}

		// When the module gets imported, make sure it sets up this slot
		load->variables.emplace_back(StmtLoadModule::VarImport{
		    .name = sourceVariableToken.contents,
		    .bindTo = slot,
		});
	} while (match(compiler, TOKEN_COMMA));

	return load;
}

// Compiles a "var" variable definition statement.
static IRNode *variableDefinition(Compiler *compiler) {
	// Grab its name, but don't declare it yet. A (local) variable shouldn't be
	// in scope in its own initializer.
	consume(compiler, TOKEN_NAME, "Expect variable name.");
	Token nameToken = compiler->parser->previous;

	IRExpr *initialiser;

	// Compile the initializer.
	if (match(compiler, TOKEN_EQ)) {
		ignoreNewlines(compiler);
		initialiser = expression(compiler);
	} else {
		// Default initialize it to null.
		initialiser = null(compiler);
	}

	// Now put it in scope.
	VarDecl *decl = declareVariable(compiler, &nameToken);
	return defineVariable(compiler, decl, initialiser);
}

// Compiles a "definition". These are the statements that bind new variables.
// They can only appear at the top level of a block and are prohibited in places
// like the non-curly body of an if or while.
IRNode *definition(Compiler *compiler) {
	std::unique_ptr<AttributePack> attributes = matchAttribute(compiler);

	if (match(compiler, TOKEN_CLASS)) {
		return classDefinition(compiler, false, std::move(attributes));
	}
	if (match(compiler, TOKEN_FOREIGN)) {
		consume(compiler, TOKEN_CLASS, "Expect 'class' after 'foreign'.");
		return classDefinition(compiler, true, std::move(attributes));
	}

	if (attributes) {
		error(compiler, "Attributes can only specified before a class or a method");
	}

	if (match(compiler, TOKEN_IMPORT)) {
		return importStatement(compiler);
	}

	if (match(compiler, TOKEN_VAR)) {
		return variableDefinition(compiler);
	}

	return statement(compiler);
}

IRFn *wrenCompile(CompContext *context, Module *mod, const char *source, bool isExpression, bool compilingCore) {
	// Skip the UTF-8 BOM if there is one.
	if (strncmp(source, "\xEF\xBB\xBF", 3) == 0)
		source += 3;

	Parser parser;
	parser.context = context;
	parser.targetModule = mod;
	parser.source = source;
	parser.compilingInternal = compilingCore;

	parser.tokenStart = source;
	parser.currentChar = source;
	parser.currentLine = 1;
	parser.numParens = 0;

	// Zero-init the current token. This will get copied to previous when
	// nextToken() is called below.
	parser.next.type = TOKEN_ERROR;
	parser.next.contents = "";
	parser.next.line = 0;
	parser.next.value = CcValue::UNDEFINED;

	parser.printErrors = true; // TODO // printErrors;
	parser.hasError = false;

	// Read the first token into next
	nextToken(&parser);
	// Copy next -> current
	nextToken(&parser);

	Compiler compiler;
	initCompiler(&compiler, &parser, NULL);
	ignoreNewlines(&compiler);

	compiler.fn->debugName = moduleName(&parser) + "::__root_func";
	compiler.fn->debugInfo.lineNumber = 1;
	mod->AddNode(compiler.fn);
	ASSERT(mod->GetFunctions().front() == compiler.fn, "Module init function is not the module's first function!");

	// Clear out the root upvalues variable, since we're not going to add it to
	// the IR and it should never have anything added to it, since everything
	// is a global at that scope.
	compiler.fn->rootBeginUpvalues = nullptr;

	if (isExpression) {
		IRExpr *expr = expression(&compiler);
		consume(&compiler, TOKEN_EOF, "Expect end of expression.");
		compiler.fn->body = compiler.New<StmtBlock>();
		compiler.AddNew<StmtReturn>(compiler.fn->body, expr);
	} else {
		StmtBlock *block = compiler.New<StmtBlock>();
		bool hitEOF = false;
		while (!match(&compiler, TOKEN_EOF) && !hitEOF) {
			IRNode *node = definition(&compiler);

			// If there is no newline, it must be the end of file on the same line.
			if (!matchLine(&compiler)) {
				consume(&compiler, TOKEN_EOF, "Expect end of file.");
				// Set a flag here rather than break-ing, since we still need to run everything
				// below, and it has a lot of continue statements.
				hitEOF = true;
			}

			// Put statements into the main function body, put everything else into the main module
			IRStmt *stmt = dynamic_cast<IRStmt *>(node);
			if (stmt)
				block->Add(stmt);
			else
				mod->AddNode(node);

			// In the case of classes, bind a new global variable to them. Eventually this should
			// be seldom used and we can use static dispatch to make static functions "just as fast as C"
			// (including all the usual caveats that apply there) and this will only be used to
			// maintain the illusion of classes being regular objects, eg for putting them in lists.
			// We also add a class definition node, which triggers the class to actually be created.
			IRClass *classDecl = dynamic_cast<IRClass *>(node);
			if (classDecl) {
				// If this is a C++ system class, don't try and define a global variable over it.
				// Instead, suffix the global variable so it's still picked up for GC purposes etc, but
				// won't cause any name collision problems.
				std::string globalName = classDecl->info->name;
				if (classDecl->info->IsCppSystemClass()) {
					globalName += "_gbl";
				}

				IRGlobalDecl *global = mod->AddVariable(globalName);

				// If the variable was already defined, check if it was implicitly defined
				if (global == nullptr) {
					IRGlobalDecl *current = mod->FindVariable(globalName);
					if (current->undeclaredLineUsed.has_value()) {
						global = current;
						global->undeclaredLineUsed = std::optional<int>();
					}
				}

				// If it explicitly defined, fail
				if (!global) {
					error(&compiler, "Module-level variable for class '%s' is already defined",
					    classDecl->info->name.c_str());
					continue;
				}

				global->targetClass = classDecl;

				StmtDefineClass *defineClass = compiler.New<StmtDefineClass>();
				defineClass->targetClass = classDecl;
				defineClass->outputVariable = global;
				block->Add(defineClass);
			}
		}
		compiler.AddNew<StmtReturn>(block, compiler.New<ExprConst>(CcValue::NULL_TYPE));
		compiler.fn->body = block;
	}

	// See if there are any implicitly declared module-level variables that never
	// got an explicit definition. They will have values that are numbers
	// indicating the line where the variable was first used.
	for (IRGlobalDecl *variable : parser.targetModule->GetGlobalVariables()) {
		if (variable->undeclaredLineUsed.has_value()) {
			// Synthesize a token for the original use site.
			parser.previous.type = TOKEN_NAME;
			parser.previous.contents = variable->Name();
			parser.previous.line = variable->undeclaredLineUsed.value();
			error(&compiler, "Variable is used but not defined.");
		}
	}

	endCompiler(&compiler, "(script)");

	if (parser.hasError)
		return nullptr;

	return compiler.fn;
}
