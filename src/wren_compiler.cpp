// Derived from the wren source code, src/vm/wren_compiler.h

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
#include "CcValue.h"
#include "ClassInfo.h"
#include "CompContext.h"
#include "ConstantsPool.h"
#include "IRNode.h"
#include "Module.h"
#include "Scope.h"
#include "SymbolTable.h"
#include "common.h"

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
};

struct Parser {
	CompContext *context;

	// The module being parsed.
	Module *module;

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
};

struct CompilerUpvalue {
	// True if this upvalue is capturing a local variable from the enclosing
	// function. False if it's capturing an upvalue.
	bool isLocal;

	// The index of the local or upvalue being captured in the enclosing function.
	int index;
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

	// Depth of the scope(s) that need to be exited if a break is hit inside the
	// loop.
	int scopeDepth;

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

	// The upvalues that this function has captured from outer scopes. The count
	// of them is stored in [numUpvalues].
	CompilerUpvalue upvalues[MAX_UPVALUES];

	// The current level of block scope nesting, where zero is no nesting. A -1
	// here means top-level code is being compiled and there is no block scope
	// in effect at all. Any variables declared will be module-level.
	int scopeDepth;

	// The current innermost loop being compiled, or NULL if not in a loop.
	Loop *loop;

	// If this is a compiler for a method, keeps track of the class enclosing it.
	ClassInfo *enclosingClass;

	// The function being compiled.
	IRFn *fn;

	// The constants for the function being compiled.
	ConstantsPool constants;

	// Whether or not the compiler is for a constructor initializer
	bool isInitializer;

	// The number of attributes seen while parsing.
	// We track this separately as compile time attributes
	// are not stored, so we can't rely on attributes->count
	// to enforce an error message when attributes are used
	// anywhere other than methods or classes.
	int numAttributes;
	// Attributes for the next class or method.
	std::unordered_map<std::string, CcValue> *attributes;

	// Utility functions:
	template <typename T, typename... Args> T *New(Args &&...args) {
		return parser->context->alloc.New<T, Args...>(std::forward<Args>(args)...);
	}

	/// Creates and adds a new statement
	/// This is just shorthand for creating the statement with compiler->New and then adding it to the block.
	template <typename T, typename... Args> T *AddNew(StmtBlock *block, Args &&...args) {
		T *value = parser->context->alloc.New<T, Args...>(std::forward<Args>(args)...);
		block->Add(value);
		return value;
	}
};

// Forward declarations

// TODO re-enable for attribute support
#if 0
static void disallowAttributes(Compiler *compiler);
static void addToAttributeGroup(Compiler *compiler, Value group, Value key, Value value);
static void emitClassAttributes(Compiler *compiler, ClassInfo *classInfo);
static void copyAttributes(Compiler *compiler, ObjMap *into);
static void copyMethodAttributes(Compiler *compiler, bool isForeign, bool isStatic, const char *fullSignature,
                                 int32_t length);
#endif

static IRExpr *null(Compiler *compiler, bool canAssign = false);

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
	fmt::print("Assertion failure at {}:{} - {}\n", file, line, msg);
	abort();
}

static std::string moduleName(Parser *parser) {
	std::optional<std::string> module = parser->module->Name();
	return module ? module.value() : "<unknown>";
}

static void printError(Parser *parser, int line, const char *label, const char *format, va_list args) {
	parser->hasError = true;
	if (!parser->printErrors)
		return;

	// Format the label and message.
	char buf[256];
	vsnprintf(buf, sizeof(buf), format, args);
	std::string message = std::string(label) + ": " + std::string(buf);

	fmt::print("WrenCC at {}:{} - {}\n", moduleName(parser), line, message);
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

// Used in IRNode.cpp as a bit of a hack
ArenaAllocator *getCompilerAlloc(Compiler *compiler) { return &compiler->parser->context->alloc; }

// Initializes [compiler].
static void initCompiler(Compiler *compiler, Parser *parser, Compiler *parent, bool isMethod) {
	compiler->parser = parser;
	compiler->parent = parent;
	compiler->loop = NULL;
	compiler->enclosingClass = NULL;
	compiler->isInitializer = false;
	compiler->fn = NULL;

	parser->context->compiler = compiler;

	// Declare the method receiver for methods, so it can be resolved like any other local variable

	if (isMethod) {
		LocalVariable *var = compiler->New<LocalVariable>();
		var->name = "this";
		var->depth = -1;
		compiler->locals.Add(var);
	}

	if (parent == NULL) {
		// Compiling top-level code, so the initial scope is module-level.
		compiler->scopeDepth = -1;
	} else {
		// The initial scope for functions and methods is local scope.
		compiler->scopeDepth = 0;
	}

	compiler->numAttributes = 0;
	compiler->fn = parser->context->alloc.New<IRFn>();
}

// Lexing ----------------------------------------------------------------------

typedef struct {
	const char *identifier;
	size_t length;
	TokenType tokenType;
} Keyword;

// The table of reserved words and their associated token types.
static Keyword keywords[] = {
    {"break", 5, TOKEN_BREAK},   {"continue", 8, TOKEN_CONTINUE},
    {"class", 5, TOKEN_CLASS},   {"construct", 9, TOKEN_CONSTRUCT},
    {"else", 4, TOKEN_ELSE},     {"false", 5, TOKEN_FALSE},
    {"for", 3, TOKEN_FOR},       {"foreign", 7, TOKEN_FOREIGN},
    {"if", 2, TOKEN_IF},         {"import", 6, TOKEN_IMPORT},
    {"as", 2, TOKEN_AS},         {"in", 2, TOKEN_IN},
    {"is", 2, TOKEN_IS},         {"null", 4, TOKEN_NULL},
    {"return", 6, TOKEN_RETURN}, {"static", 6, TOKEN_STATIC},
    {"super", 5, TOKEN_SUPER},   {"this", 4, TOKEN_THIS},
    {"true", 4, TOKEN_TRUE},     {"var", 3, TOKEN_VAR},
    {"while", 5, TOKEN_WHILE},   {NULL, 0, TOKEN_EOF} // Sentinel to mark the end of the array.
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

	if (firstNewline != -1 && skipStart == firstNewline)
		offset = firstNewline + 1;

	if (offset > (int)string.size()) {
		parser->next.value = "";
	} else {
		parser->next.value = string.substr(offset);
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
	local->depth = compiler->scopeDepth;

	if (!compiler->locals.Add(local))
		return nullptr;

	compiler->fn->locals.push_back(local);

	return local;
}

// Create a new temporary (for compiler use only) local variable with the
// name [debugName], though this shouldn't ever be shown to the user, doesn't
// have to be unique and doesn't have to be a valid identifier.
static LocalVariable *addTemporary(Compiler *compiler, const std::string &debugName) {
	LocalVariable *local = compiler->New<LocalVariable>();
	local->name = debugName;
	local->depth = 0;
	compiler->fn->temporaries.push_back(local);
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
		IRGlobalDecl *global = compiler->parser->module->AddVariable(token->contents);

		// If the variable was already defined, check if it was implicitly defined
		if (global == nullptr) {
			IRGlobalDecl *current = compiler->parser->module->FindVariable(token->contents);
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
		}

		// If it was already explicitly defined, throw an error
		if (global == nullptr) {
			error(compiler, "Module variable is already defined.");
			return nullptr;
		}

		compiler->parser->module->AddNode(global);

		// } else if (symbol == -3) {
		// }

		return global;
	}

	LocalVariable *var = addLocal(compiler, token->contents);

	// Adding a variable only fails if the variable already exists in the scope. If the variable exists in the
	// parent scope, it's not a problem as we shadow it.
	if (!var) {
		error(compiler, "Variable is already declared in this scope.");
		return nullptr;
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
static void pushScope(Compiler *compiler) {
	compiler->scopeDepth++;
	compiler->locals.PushFrame();
}

// Generates code to discard local variables at [depth] or greater. Does *not*
// actually undeclare variables or pop any scopes, though. This is called
// directly when compiling "break" statements to ditch the local variables
// before jumping out of the loop even though they are still in scope *past*
// the break instruction.
static void discardLocals(Compiler *compiler, int depth) {
	ASSERT(compiler->scopeDepth > -1, "Cannot exit top-level scope.");

	// Keeping this around to close upvalues, if that's how we end up implementing it

	// int local = compiler->numLocals - 1;
	// while (local >= 0 && compiler->locals[local].depth >= depth) {
	// 	// If the local was closed over, make sure the upvalue gets closed when it
	// 	// goes out of scope on the stack. We use emitByte() and not emitOp() here
	// 	// because we don't want to track that stack effect of these pops since the
	// 	// variables are still in scope after the break.
	// 	if (compiler->locals[local].isUpvalue) {
	// 		emitByte(compiler, CODE_CLOSE_UPVALUE);
	// 	} else {
	// 		emitByte(compiler, CODE_POP);
	// 	}

	// 	local--;
	// }

	// return compiler->numLocals - local - 1;
}

// Closes the last pushed block scope and discards any local variables declared
// in that scope. This should only be called in a statement context where no
// temporaries are still on the stack.
static void popScope(Compiler *compiler) {
	discardLocals(compiler, compiler->scopeDepth);
	compiler->scopeDepth--;
	compiler->locals.PopFrame();
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
static VarDecl *findUpvalue(Compiler *compiler, const std::string &name) {
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
	VarDecl *upvalue = findUpvalue(compiler->parent, name);
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

// Look up [name] in the current scope to see what variable it refers to.
// Returns the variable either in module scope, local scope, or the enclosing
// function's upvalue list. Returns a variable with index -1 if not found.
static VarDecl *resolveName(Compiler *compiler, const std::string &name) {
	VarDecl *variable = resolveNonmodule(compiler, name);
	if (variable)
		return variable;

	// Load a module-level variable
	abort(); // TODO

	// variable.scope = SCOPE_MODULE;
	// variable.index = wrenSymbolTableFind(&compiler->parser->module->variableNames, name, length);
	// return variable;
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
		IRNode *node = definition(compiler);
		consumeLine(compiler, "Expect newline after statement.");

		if (!node)
			continue;

		// Don't support classes declared in blocks etc
		IRStmt *stmt = dynamic_cast<IRStmt *>(node);
		ASSERT(stmt, "Only statements are supported inside non-expression functions");
		block->Add(stmt);
	} while (peek(compiler) != TOKEN_RIGHT_BRACE && peek(compiler) != TOKEN_EOF);

	consume(compiler, TOKEN_RIGHT_BRACE, "Expect '}' at end of block.");
	return block;
}

// Parses a method or function body, after the initial "{" has been consumed.
//
// If [Compiler->isInitializer] is `true`, this is the body of a constructor
// initializer. In that case, this adds the code to ensure it returns `this`.
static IRStmt *finishBody(Compiler *compiler) {
	IRNode *body = finishBlock(compiler);

	IRExpr *expr = dynamic_cast<IRExpr *>(body);
	IRStmt *stmt = dynamic_cast<IRStmt *>(body);
	bool isExpressionBody = expr != nullptr;

	ASSERT(expr || stmt, "The contents of a block must either be a statement or an expression");

	IRExpr *returnValue = nullptr;
	StmtBlock *block = compiler->New<StmtBlock>();

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

	block->Add(compiler->New<StmtReturn>(returnValue));
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

		compiler->fn->parameters.push_back(local);
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

	toAdd->Add(compiler, call);
	return nullptr;
}

// Compiles an (optional) argument list for a method call with [methodSignature]
// and then calls it.
static IRExpr *methodCall(Compiler *compiler, bool super, Signature *signature, IRExpr *receiver) {
	// Make a new signature that contains the updated arity and type based on
	// the arguments we find.
	Signature called = {signature->name, SIG_GETTER, 0};

	std::vector<IRExpr *> args;

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
		initCompiler(&fnCompiler, compiler->parser, compiler, false);
		std::string subDebugName = signature->name + "_" + std::to_string(compiler->parser->previous.line);
		fnCompiler.fn->debugName = compiler->fn->debugName + "::" + subDebugName;
		compiler->parser->module->AddNode(fnCompiler.fn);

		// Make a dummy signature to track the arity.
		Signature fnSignature = {"", SIG_METHOD, 0};

		// Parse the parameter list, if any.
		if (match(compiler, TOKEN_PIPE)) {
			finishParameterList(&fnCompiler, &fnSignature);
			consume(compiler, TOKEN_PIPE, "Expect '|' after function parameters.");
		}

		fnCompiler.fn->arity = fnSignature.arity;

		fnCompiler.fn->body = finishBody(&fnCompiler);

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

	ExprFuncCall *call = compiler->New<ExprFuncCall>();
	call->receiver = receiver;
	call->signature = normaliseSignature(compiler, called);
	call->args = std::move(args);
	call->super = super;
	return call;
}

// Compiles a call whose name is the previously consumed token. This includes
// getters, method calls with arguments, and setter calls.
static IRExpr *namedCall(Compiler *compiler, IRExpr *receiver, bool canAssign, bool super) {
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
		call->super = super;
		call->args.push_back(value);
		return call;
	}

	IRExpr *call = methodCall(compiler, super, &signature, receiver);
	allowLineBeforeDot(compiler);
	return call;
}

// Emits the code to load [variable] onto the stack.
static IRExpr *loadVariable(Compiler *compiler, VarDecl *variable) { return compiler->New<ExprLoad>(variable); }

// Loads the receiver of the currently enclosing method. Correctly handles
// functions defined inside methods.
static IRExpr *loadThis(Compiler *compiler) { return loadVariable(compiler, resolveNonmodule(compiler, "this")); }

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
	LocalVariable *list = addTemporary(compiler, "list-builder");
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
	LocalVariable *map = addTemporary(compiler, "map-builder");
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
		parsePrecedence(compiler, PREC_UNARY);
		consume(compiler, TOKEN_COLON, "Expect ':' after map key.");
		ignoreNewlines(compiler);

		// The value.
		IRExpr *toAdd = expression(compiler);
		callMethod(compiler, "[_]=(_)", loadVariable(compiler, map), {toAdd}, block);
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

	// If there's an "=" after a field name, it's an assignment.
	if (canAssign && match(compiler, TOKEN_EQ)) {
		// Compile the right-hand side.
		IRExpr *value = expression(compiler);

		// Assignments in Wren can be used as expressions themselves, so we have to return an expression
		ExprRunStatements *run = compiler->New<ExprRunStatements>();
		LocalVariable *temporary = addTemporary(compiler, "set-field-expr");
		StmtBlock *block = compiler->New<StmtBlock>();
		run->statement = block;
		run->temporary = temporary;

		block->Add(compiler->New<StmtAssign>(temporary, value));
		block->Add(compiler->New<StmtFieldAssign>(field, loadVariable(compiler, temporary)));

		return run;
	}

	allowLineBeforeDot(compiler);

	return compiler->New<ExprFieldLoad>(field);
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
		LocalVariable *tmp = addTemporary(compiler, "bareName-assign-" + variable->Name());
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

	// If this is the first time we've seen this static field, implicitly
	// define it as a variable in the scope surrounding the class definition.
	VarDecl *local = compiler->locals.Lookup(token->contents);
	if (local == nullptr) {
		local = declareVariable(classCompiler, NULL);

		// TODO null initialisation
	}

	// It definitely exists now, so resolve it properly. This is different from
	// the above resolveLocal() call because we may have already closed over it
	// as an upvalue.
	VarDecl *variable = resolveName(compiler, token->contents);
	return bareName(compiler, canAssign, variable);
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
		return namedCall(compiler, loadThis(compiler), canAssign, false);
	}

	// Check if a system variable with the same name exists (eg, for Object)
	if (ExprSystemVar::SYSTEM_VAR_NAMES.contains(token->contents))
		return compiler->New<ExprSystemVar>(token->contents);

	// Otherwise, look for a module-level variable with the name.
	variable = compiler->parser->module->FindVariable(token->contents);
	if (variable == nullptr) {
		// Implicitly define a module-level variable in the hopes that we get a real definition later.
		IRGlobalDecl *decl = compiler->parser->module->AddVariable(token->contents);
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
	LocalVariable *parts = addTemporary(compiler, "string-interpolation-parts");
	LocalVariable *result = addTemporary(compiler, "string-interpolation-result");
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
	ClassInfo *enclosingClass = getEnclosingClass(compiler);
	if (enclosingClass == NULL) {
		error(compiler, "Cannot use 'super' outside of a method.");
	}

	// TODO: Super operator calls.
	// TODO: There's no syntax for invoking a superclass constructor with a
	// different name from the enclosing one. Figure that out.

	// See if it's a named super call, or an unnamed one.
	if (match(compiler, TOKEN_DOT)) {
		// Compile the superclass call.
		consume(compiler, TOKEN_NAME, "Expect method name after 'super.'.");
		return namedCall(compiler, loadThis(compiler), canAssign, true);
	}

	// No explicit name, so use the name of the enclosing method. Make sure we
	// check that enclosingClass isn't NULL first. We've already reported the
	// error, but we don't want to crash here.
	return methodCall(compiler, true, enclosingClass->signature, loadThis(compiler));
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
	return namedCall(compiler, lhs, canAssign, false);
}

static IRExpr *and_(Compiler *compiler, IRExpr *lhs, bool canAssign) {
	ignoreNewlines(compiler);

	// We have to run a series of statements including jumps to find
	// the value.
	StmtBlock *block = compiler->New<StmtBlock>();
	LocalVariable *tmp = addTemporary(compiler, "and-value-tmp");
	ExprRunStatements *wrapper = compiler->New<ExprRunStatements>();
	wrapper->statement = block;
	wrapper->temporary = tmp;

	// Put the LHS in the temporary
	compiler->AddNew<StmtAssign>(block, tmp, lhs);

	// Skip the right argument if the left is false.
	StmtJump *jump = compiler->AddNew<StmtJump>(block);
	jump->condition = loadVariable(compiler, tmp);
	jump->jumpOnFalse = true;
	IRExpr *rhs = parsePrecedence(compiler, PREC_LOGICAL_AND);
	compiler->AddNew<StmtAssign>(block, tmp, rhs);
	jump->target = compiler->AddNew<StmtLabel>(block, "and-target");

	return wrapper;
}

static IRExpr *or_(Compiler *compiler, IRExpr *lhs, bool canAssign) {
	ignoreNewlines(compiler);

	// We have to run a series of statements including jumps to find
	// the value.
	StmtBlock *block = compiler->New<StmtBlock>();
	LocalVariable *tmp = addTemporary(compiler, "or-value-tmp");
	ExprRunStatements *wrapper = compiler->New<ExprRunStatements>();
	wrapper->statement = block;
	wrapper->temporary = tmp;

	// Put the LHS in the temporary
	compiler->AddNew<StmtAssign>(block, tmp, lhs);

	// Skip the right argument if the left is true.
	StmtJump *jump = compiler->AddNew<StmtJump>(block);
	jump->condition = loadVariable(compiler, tmp);
	IRExpr *rhs = parsePrecedence(compiler, PREC_LOGICAL_OR);
	compiler->AddNew<StmtAssign>(block, tmp, rhs);
	jump->target = compiler->AddNew<StmtLabel>(block, "or-target");

	return wrapper;
}

static IRExpr *conditional(Compiler *compiler, IRExpr *condition, bool canAssign) {
	// Ignore newline after '?'.
	ignoreNewlines(compiler);

	// We have to run a series of statements including jumps to find
	// the value.
	StmtBlock *block = compiler->New<StmtBlock>();
	LocalVariable *tmp = addTemporary(compiler, "conditional-value-tmp");
	ExprRunStatements *wrapper = compiler->New<ExprRunStatements>();
	wrapper->statement = block;
	wrapper->temporary = tmp;

	// If the condition is false, jump to the 2nd term
	StmtJump *jumpToFalse = compiler->AddNew<StmtJump>(block);
	jumpToFalse->condition = loadVariable(compiler, tmp);
	jumpToFalse->jumpOnFalse = true;

	// Compile the then branch.
	IRExpr *trueValue = parsePrecedence(compiler, PREC_CONDITIONAL);
	compiler->AddNew<StmtAssign>(block, tmp, trueValue);

	consume(compiler, TOKEN_COLON, "Expect ':' after then branch of conditional operator.");
	ignoreNewlines(compiler);

	// Jump over the else branch when the if branch is taken.
	StmtJump *trueToEnd = compiler->AddNew<StmtJump>(block);

	// Compile the else branch.
	jumpToFalse->target = compiler->AddNew<StmtLabel>(block, "condition-start-to-else");

	parsePrecedence(compiler, PREC_ASSIGNMENT);

	// Patch the jump over the else.
	trueToEnd->target = compiler->AddNew<StmtLabel>(block, "condition-true-to-end");

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
	declareNamedVariable(compiler);
	consume(compiler, TOKEN_RIGHT_PAREN, "Expect ')' after parameter name.");
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
		declareNamedVariable(compiler);
		consume(compiler, TOKEN_RIGHT_PAREN, "Expect ')' after parameter name.");
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
	declareNamedVariable(compiler);
	consume(compiler, TOKEN_RIGHT_PAREN, "Expect ')' after parameter name.");

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
		return nullptr;
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
	loop->scopeDepth = compiler->scopeDepth;
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

	// Create a scope for the hidden local variables used for the iterator.
	pushScope(compiler);
	StmtBlock *block = compiler->New<StmtBlock>();

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
	pushScope(compiler);
	LocalVariable *valueVar = addLocal(compiler, name);
	block->Add(compiler->New<StmtAssign>(valueVar, valueCall));

	block->Add(loopBody(compiler));

	// Loop variable.
	popScope(compiler);

	block->Add(endLoop(compiler));

	// Hidden variables.
	popScope(compiler);

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
		discardLocals(compiler, compiler->loop->scopeDepth + 1);

		// Add a placeholder jump. We don't yet know what the end-of-loop label is (it
		// won't have been created yet), so make it jump to nullptr and add it to the
		// list of jumps to be fixed up by endLoop.
		StmtJump *jump = compiler->New<StmtJump>();
		compiler->loop->exitJumps.push_back(jump);
		return jump;
	}
	if (match(compiler, TOKEN_CONTINUE)) {
		if (compiler->loop == NULL) {
			error(compiler, "Cannot use 'continue' outside of a loop.");
			return nullptr;
		}

		// Since we will be jumping out of the scope, make sure any locals in it
		// are discarded first.
		discardLocals(compiler, compiler->loop->scopeDepth + 1);

		// emit a jump back to the top of the loop
		return compiler->New<StmtJump>(compiler->loop->start, nullptr);
	}
	if (match(compiler, TOKEN_FOR)) {
		return forStatement(compiler);
	}
	if (match(compiler, TOKEN_IF)) {
		return ifStatement(compiler);
	}
	if (match(compiler, TOKEN_RETURN)) {
		// Compile the return value.
		if (peek(compiler) == TOKEN_LINE) {
			// If there's no expression after return, initializers should
			// return 'this' and regular methods should return null
			IRExpr *returnValue = compiler->isInitializer ? loadThis(compiler) : null(compiler);
			return compiler->New<StmtReturn>(returnValue);
		}

		if (compiler->isInitializer) {
			error(compiler, "A constructor cannot return a value.");
		}

		IRExpr *returnValue = expression(compiler);
		return compiler->New<StmtReturn>(returnValue);
	}
	if (match(compiler, TOKEN_WHILE)) {
		return whileStatement(compiler);
	}
	if (match(compiler, TOKEN_LEFT_BRACE)) {
		// Block statement.
		pushScope(compiler);

		IRNode *body = finishBlock(compiler);

		IRStmt *stmt = dynamic_cast<IRStmt *>(body);

		// Ignore nulls, they mean 'ignore this'
		if (body && !stmt) {
			// Block was an expression, so discard it.
			IRExpr *expr = dynamic_cast<IRExpr *>(body);
			ASSERT(expr, "finishBlock returned neither a statement nor an expression");
			stmt = compiler->New<StmtEvalAndIgnore>(expr);
		}
		popScope(compiler);

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

static bool matchAttribute(Compiler *compiler) {
	// For now, attributes aren't implemented
#if 0

	if (match(compiler, TOKEN_HASH)) {
		compiler->numAttributes++;
		bool runtimeAccess = match(compiler, TOKEN_BANG);
		if (match(compiler, TOKEN_NAME)) {
			CcValue group = compiler->parser->previous.value;
			TokenType ahead = peek(compiler);
			if (ahead == TOKEN_EQ || ahead == TOKEN_LINE) {
				CcValue key = group;
				CcValue value = NULL_VAL;
				if (match(compiler, TOKEN_EQ)) {
					value = consumeLiteral(compiler,
					                       "Expect a Bool, Num, String or Identifier literal for an attribute value.");
				}
				if (runtimeAccess)
					addToAttributeGroup(compiler, NULL_VAL, key, value);
			} else if (match(compiler, TOKEN_LEFT_PAREN)) {
				ignoreNewlines(compiler);
				if (match(compiler, TOKEN_RIGHT_PAREN)) {
					error(compiler, "Expected attributes in group, group cannot be empty.");
				} else {
					while (peek(compiler) != TOKEN_RIGHT_PAREN) {
						consume(compiler, TOKEN_NAME, "Expect name for attribute key.");
						CcValue key = compiler->parser->previous.value;
						CcValue value = NULL_VAL;
						if (match(compiler, TOKEN_EQ)) {
							value = consumeLiteral(
							    compiler, "Expect a Bool, Num, String or Identifier literal for an attribute value.");
						}
						if (runtimeAccess)
							addToAttributeGroup(compiler, group, key, value);
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
		return true;
	}
#endif

	return false;
}

// Compiles a method definition inside a class body.
//
// Returns `true` if it compiled successfully, or `false` if the method couldn't
// be parsed.
static bool method(Compiler *compiler, IRClass *classNode) {
	// Parse any attributes before the method and store them
	if (matchAttribute(compiler)) {
		return method(compiler, classNode);
	}

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
	initCompiler(&methodCompiler, compiler->parser, compiler, true);

	// Compile the method signature. This might change the signature, so we have to normalise it afterwards.
	signatureFn(&methodCompiler, &tmpSignature);
	Signature *signature = normaliseSignature(compiler, tmpSignature);
	compiler->enclosingClass->signature = signature;
	methodCompiler.fn->debugName = classNode->info->name + "::" + signature->ToString();

	methodCompiler.isInitializer = signature->type == SIG_INITIALIZER;

	if (isStatic && signature->type == SIG_INITIALIZER) {
		error(compiler, "A constructor cannot be static.");
	}

	// Copy any attributes the compiler collected into the enclosing class
	// TODO re-enable when signatures are implemented
	// copyMethodAttributes(compiler, isForeign, isStatic, fullSignature, length);

	// Check for duplicate methods. Doesn't matter that it's already been
	// defined, error will discard bytecode anyway.
	// Check if the method table already contains this symbol
	MethodInfo *method = declareMethod(compiler, signature, false);
	method->isForeign = isForeign;

	if (isForeign) {
		// We don't need the function we started compiling in the parameter list
		// any more. This is normally done by endCompiler, but we don't need to call that.
		methodCompiler.parser->context->compiler = methodCompiler.parent;
	} else {
		consume(compiler, TOKEN_LEFT_BRACE, "Expect '{' to begin method body.");
		methodCompiler.fn->body = finishBody(&methodCompiler);
		method->fn = methodCompiler.fn;
		method->fn->enclosingClass = classNode;
		endCompiler(&methodCompiler, signature->ToString());
		compiler->parser->module->AddNode(method->fn);
	}

	if (signature->type == SIG_INITIALIZER) {
		// Also define a matching constructor method on the metaclass.
		// Constructors have to parts, kinda like C++'s destructors: one initialises the
		// memory and runs the user's code, and that's of type SIG_INITIALISER.
		// There's a regular static method with (aside from being SIG_METHOD) the same signature,
		// which first allocates the memory to store the object then calls the initialiser.
		Signature allocSignatureValue = *signature;
		allocSignatureValue.type = SIG_METHOD;
		MethodInfo *alloc = declareMethod(compiler, normaliseSignature(compiler, allocSignatureValue), true);

		alloc->isForeign = false; // For foreign classes, the allocate memory node handles the FFI stuff.

		IRFn *fn = compiler->New<IRFn>();
		compiler->parser->module->AddNode(fn);
		fn->arity = signature->arity;
		fn->enclosingClass = classNode;
		fn->debugName = method->fn->debugName + "::alloc";
		alloc->fn = fn;

		StmtBlock *block = compiler->New<StmtBlock>();
		fn->body = block;

		LocalVariable *objLocal = compiler->New<LocalVariable>();
		objLocal->name = "alloc_temp_special";
		objLocal->depth = 0;
		fn->locals.push_back(objLocal);

		// First, allocate the memory of the new instance
		ExprAllocateInstanceMemory *allocExpr = compiler->New<ExprAllocateInstanceMemory>();
		allocExpr->target = classNode;

		block->Add(compiler->New<StmtAssign>(objLocal, allocExpr));

		// Now run the initialiser
		ExprFuncCall *call = compiler->New<ExprFuncCall>();
		call->signature = signature;
		call->receiver = compiler->New<ExprLoad>(objLocal);
		// TODO arguments
		block->Add(compiler, call);

		// Finally, return it
		block->Add(compiler->New<StmtReturn>(compiler->New<ExprLoad>(objLocal)));
	}

	return true;
}

// Compiles a class definition. Assumes the "class" token has already been
// consumed (along with a possibly preceding "foreign" token).
static IRNode *classDefinition(Compiler *compiler, bool isForeign) {
	IRClass *classNode = compiler->New<IRClass>();
	classNode->info = std::make_unique<ClassInfo>();

	// Find the class's name
	consume(compiler, TOKEN_NAME, "Expect class name.");
	std::string className = compiler->parser->previous.contents;

	// Load the superclass (if there is one).
	if (match(compiler, TOKEN_IS)) {
		parsePrecedence(compiler, PREC_CALL);
	} else {
		// Implicitly inherit from Object.
		loadCoreVariable(compiler, "Object");
	}

	// Push a local variable scope. Static fields in a class body are hoisted out
	// into local variables declared in this scope. Methods that use them will
	// have upvalues referencing them.
	pushScope(compiler);

	ClassInfo &classInfo = *classNode->info;
	classInfo.isForeign = isForeign;
	classInfo.name = className;

	// Copy any existing attributes into the class
	// TODO enable this when attributes are implemented
	// copyAttributes(compiler, classInfo.classAttributes);

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

	// If any attributes are present,
	// instantiate a ClassAttributes instance for the class
	// and send it over to CODE_END_CLASS
	// TODO re-enable for attribute support
#if 0
	bool hasAttr = classInfo.classAttributes != NULL || classInfo.methodAttributes != NULL;
	if (hasAttr) {
		emitClassAttributes(compiler, &classInfo);
		loadVariable(compiler, classVariable);
		// At the moment, we don't have other uses for CODE_END_CLASS,
		// so we put it inside this condition. Later, we can always
		// emit it and use it as needed.
		emitOp(compiler, CODE_END_CLASS);
	}
#endif

	compiler->enclosingClass = NULL;
	popScope(compiler);

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
static IRNode *import(Compiler *compiler) {
	ignoreNewlines(compiler);
	consume(compiler, TOKEN_STRING, "Expect a string after 'import'.");
	std::string moduleName = compiler->parser->previous.contents;

	// Add a node to say that we need the module. This is kinda just put anywhere inside the
	// file, with no concerns for ordering or matching up with if statements or anything like that.
	IRImport *import = compiler->New<IRImport>();
	import->moduleName = moduleName;
	compiler->parser->module->AddNode(import);

	// At this point in the execution, require that the module is loaded if it wasn't already
	// This makes stuff like requiring modules inside if statements work properly. If you have
	// a debug module that throws an error when imported in release builds and all the imports
	// of it check if you're in release mode or not, then this is what makes that work properly.
	StmtLoadModule *load = compiler->New<StmtLoadModule>();

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
	if (matchAttribute(compiler)) {
		return definition(compiler);
	}

	if (match(compiler, TOKEN_CLASS)) {
		return classDefinition(compiler, false);
	}
	if (match(compiler, TOKEN_FOREIGN)) {
		consume(compiler, TOKEN_CLASS, "Expect 'class' after 'foreign'.");
		return classDefinition(compiler, true);
	}

	// TODO enable for attributes
	// disallowAttributes(compiler);

	if (match(compiler, TOKEN_IMPORT)) {
		return import(compiler);
	}

	if (match(compiler, TOKEN_VAR)) {
		return variableDefinition(compiler);
	}

	return statement(compiler);
}

IRFn *wrenCompile(CompContext *context, Module *module, const char *source, bool isExpression) {
	// Skip the UTF-8 BOM if there is one.
	if (strncmp(source, "\xEF\xBB\xBF", 3) == 0)
		source += 3;

	Parser parser;
	parser.context = context;
	parser.module = module;
	parser.source = source;

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
	initCompiler(&compiler, &parser, NULL, false);
	ignoreNewlines(&compiler);

	compiler.fn->debugName = moduleName(&parser) + "::__root_func";
	module->AddNode(compiler.fn);
	ASSERT(module->GetFunctions().front() == compiler.fn, "Module init function is not the module's first function!");

	if (isExpression) {
		IRExpr *expr = expression(&compiler);
		consume(&compiler, TOKEN_EOF, "Expect end of expression.");
		compiler.fn->body = compiler.New<StmtReturn>(expr);
	} else {
		StmtBlock *block = compiler.New<StmtBlock>();
		while (!match(&compiler, TOKEN_EOF)) {
			IRNode *node = definition(&compiler);

			// If there is no newline, it must be the end of file on the same line.
			if (!matchLine(&compiler)) {
				consume(&compiler, TOKEN_EOF, "Expect end of file.");
				break;
			}

			// Put statements into the main function body, put everything else into the main module
			IRStmt *stmt = dynamic_cast<IRStmt *>(node);
			if (stmt)
				block->Add(stmt);
			else
				module->AddNode(node);

			// In the case of classes, bind a new global variable to them. Eventually this should
			// be seldom used and we can use static dispatch to make static functions "just as fast as C"
			// (including all the usual caveats that apply there) and this will only be used to
			// maintain the illusion of classes being regular objects, eg for putting them in lists.
			IRClass *classDecl = dynamic_cast<IRClass *>(node);
			if (classDecl) {
				IRGlobalDecl *global = module->AddVariable(classDecl->info->name);

				// If the variable was already defined, check if it was implicitly defined
				if (global == nullptr) {
					IRGlobalDecl *current = module->FindVariable(classDecl->info->name);
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

				ExprGetClassVar *classVar = compiler.New<ExprGetClassVar>();
				classVar->name = classDecl->info->name;

				block->Add(compiler.New<StmtAssign>(global, classVar));
			}
		}
		compiler.AddNew<StmtReturn>(block, compiler.New<ExprConst>(CcValue::NULL_TYPE));
		compiler.fn->body = block;
	}

	// See if there are any implicitly declared module-level variables that never
	// got an explicit definition. They will have values that are numbers
	// indicating the line where the variable was first used.
	for (IRGlobalDecl *variable : parser.module->GetGlobalVariables()) {
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

// Attribute handling, disabled for now - they're experimental upstream anyway, and can be added later
#if 0
// Helpers for Attributes

// Throw an error if any attributes were found preceding,
// and clear the attributes so the error doesn't keep happening.
static void disallowAttributes(Compiler *compiler) {
	if (compiler->numAttributes > 0) {
		error(compiler, "Attributes can only specified before a class or a method");
		wrenMapClear(compiler->parser->context, compiler->attributes);
		compiler->numAttributes = 0;
	}
}

// Add an attribute to a given group in the compiler attribues map
static void addToAttributeGroup(Compiler *compiler, Value group, Value key, Value value) {
	CompContext *vm = compiler->parser->context;

	if (IS_OBJ(group))
		wrenPushRoot(vm, AS_OBJ(group));
	if (IS_OBJ(key))
		wrenPushRoot(vm, AS_OBJ(key));
	if (IS_OBJ(value))
		wrenPushRoot(vm, AS_OBJ(value));

	Value groupMapValue = wrenMapGet(compiler->attributes, group);
	if (IS_UNDEFINED(groupMapValue)) {
		groupMapValue = OBJ_VAL(wrenNewMap(vm));
		wrenMapSet(vm, compiler->attributes, group, groupMapValue);
	}

	// we store them as a map per so we can maintain duplicate keys
	// group = { key:[value, ...], }
	ObjMap *groupMap = AS_MAP(groupMapValue);

	// var keyItems = group[key]
	// if(!keyItems) keyItems = group[key] = []
	Value keyItemsValue = wrenMapGet(groupMap, key);
	if (IS_UNDEFINED(keyItemsValue)) {
		keyItemsValue = OBJ_VAL(wrenNewList(vm, 0));
		wrenMapSet(vm, groupMap, key, keyItemsValue);
	}

	// keyItems.add(value)
	ObjList *keyItems = AS_LIST(keyItemsValue);
	wrenValueBufferWrite(vm, &keyItems->elements, value);

	if (IS_OBJ(group))
		wrenPopRoot(vm);
	if (IS_OBJ(key))
		wrenPopRoot(vm);
	if (IS_OBJ(value))
		wrenPopRoot(vm);
}

// Emit the attributes in the give map onto the stack
static void emitAttributes(Compiler *compiler, ObjMap *attributes) {
	// Instantiate a new map for the attributes
	loadCoreVariable(compiler, "Map");
	callMethod(compiler, 0, "new()", 5);

	// The attributes are stored as group = { key:[value, value, ...] }
	// so our first level is the group map
	for (uint32_t groupIdx = 0; groupIdx < attributes->capacity; groupIdx++) {
		const MapEntry *groupEntry = &attributes->entries[groupIdx];
		if (IS_UNDEFINED(groupEntry->key))
			continue;
		// group key
		emitConstant(compiler, groupEntry->key);

		// group value is gonna be a map
		loadCoreVariable(compiler, "Map");
		callMethod(compiler, 0, "new()", 5);

		ObjMap *groupItems = AS_MAP(groupEntry->value);
		for (uint32_t itemIdx = 0; itemIdx < groupItems->capacity; itemIdx++) {
			const MapEntry *itemEntry = &groupItems->entries[itemIdx];
			if (IS_UNDEFINED(itemEntry->key))
				continue;

			emitConstant(compiler, itemEntry->key);
			// Attribute key value, key = []
			loadCoreVariable(compiler, "List");
			callMethod(compiler, 0, "new()", 5);
			// Add the items to the key list
			ObjList *items = AS_LIST(itemEntry->value);
			for (int itemIdx = 0; itemIdx < items->elements.count; ++itemIdx) {
				emitConstant(compiler, items->elements.data[itemIdx]);
				callMethod(compiler, 1, "addCore_(_)", 11);
			}
			// Add the list to the map
			callMethod(compiler, 2, "addCore_(_,_)", 13);
		}

		// Add the key/value to the map
		callMethod(compiler, 2, "addCore_(_,_)", 13);
	}
}

// Methods are stored as method <-> attributes, so we have to have
// an indirection to resolve for methods
static void emitAttributeMethods(Compiler *compiler, ObjMap *attributes) {
	// Instantiate a new map for the attributes
	loadCoreVariable(compiler, "Map");
	callMethod(compiler, 0, "new()", 5);

	for (uint32_t methodIdx = 0; methodIdx < attributes->capacity; methodIdx++) {
		const MapEntry *methodEntry = &attributes->entries[methodIdx];
		if (IS_UNDEFINED(methodEntry->key))
			continue;
		emitConstant(compiler, methodEntry->key);
		ObjMap *attributeMap = AS_MAP(methodEntry->value);
		emitAttributes(compiler, attributeMap);
		callMethod(compiler, 2, "addCore_(_,_)", 13);
	}
}

// Emit the final ClassAttributes that exists at runtime
static void emitClassAttributes(Compiler *compiler, ClassInfo *classInfo) {
	loadCoreVariable(compiler, "ClassAttributes");

	classInfo->classAttributes ? emitAttributes(compiler, classInfo->classAttributes) : null(compiler, false);

	classInfo->methodAttributes ? emitAttributeMethods(compiler, classInfo->methodAttributes) : null(compiler, false);

	callMethod(compiler, 2, "new(_,_)", 8);
}

// Copy the current attributes stored in the compiler into a destination map
// This also resets the counter, since the intent is to consume the attributes
static void copyAttributes(Compiler *compiler, ObjMap *into) {
	compiler->numAttributes = 0;

	if (compiler->attributes->count == 0)
		return;
	if (into == NULL)
		return;

	CompContext *vm = compiler->parser->context;

	// Note we copy the actual values as is since we'll take ownership
	// and clear the original map
	for (uint32_t attrIdx = 0; attrIdx < compiler->attributes->capacity; attrIdx++) {
		const MapEntry *attrEntry = &compiler->attributes->entries[attrIdx];
		if (IS_UNDEFINED(attrEntry->key))
			continue;
		wrenMapSet(vm, into, attrEntry->key, attrEntry->value);
	}

	wrenMapClear(vm, compiler->attributes);
}

// Copy the current attributes stored in the compiler into the method specific
// attributes for the current enclosingClass.
// This also resets the counter, since the intent is to consume the attributes
static void copyMethodAttributes(Compiler *compiler, bool isForeign, bool isStatic, const char *fullSignature,
                                 int32_t length) {
	compiler->numAttributes = 0;

	if (compiler->attributes->count == 0)
		return;

	CompContext *vm = compiler->parser->context;

	// Make a map for this method to copy into
	ObjMap *methodAttr = wrenNewMap(vm);
	wrenPushRoot(vm, (Obj *)methodAttr);
	copyAttributes(compiler, methodAttr);

	// Include 'foreign static ' in front as needed
	int32_t fullLength = length;
	if (isForeign)
		fullLength += 8;
	if (isStatic)
		fullLength += 7;
	char fullSignatureWithPrefix[MAX_METHOD_SIGNATURE + 8 + 7];
	const char *foreignPrefix = isForeign ? "foreign " : "";
	const char *staticPrefix = isStatic ? "static " : "";
	sprintf(fullSignatureWithPrefix, "%s%s%.*s", foreignPrefix, staticPrefix, length, fullSignature);
	fullSignatureWithPrefix[fullLength] = '\0';

	if (compiler->enclosingClass->methodAttributes == NULL) {
		compiler->enclosingClass->methodAttributes = wrenNewMap(vm);
	}

	// Store the method attributes in the class map
	Value key = wrenNewStringLength(vm, fullSignatureWithPrefix, fullLength);
	wrenMapSet(vm, compiler->enclosingClass->methodAttributes, key, OBJ_VAL(methodAttr));

	wrenPopRoot(vm);
}
#endif
