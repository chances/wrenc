// The infix function calls - from https://wren.io/syntax.html
// These are in the order of associativity, precedence, and the symbols.
// NOTE: these precedences are the opposite of tree-sitter's precedence!
//  In the Wren numbers, higher means it binds more loosely.
let infix_operators = [
	['left',   3, '*', '/', '%'],
	['left',   4, '+', '-'],
	['left',   5, '..', '...'],
	['left',   6, '<<', '>>'],
	['left',   7, '&'],
	['left',   8, '^'],
	['left',   9, '|'],
	['left',  10, '<', '<=', '>', '>='],
	['left',  11, 'is'],
	['left',  12, '==', '!='],
	['left',  13, '&&'],
	['left',  14, '||'],
];

// The prefix (unary) operators - these are all precedence 2, and
// are left-associative.
let prefix_operators = ['!', '-', '~'];

// Make a list of all the possible operators, so they can be used in
// method definitions.
let all_operators
{
	let tmp = new Set();
	for (let operator of infix_operators) {
		for (var i=2; i<operator.length; i++) {
			tmp.add(operator[i]);
		}
	}
	for (let operator of prefix_operators) {
		tmp.add(operator);
	}
	all_operators = Array.from(tmp);
}

// Fix the direction problem with Wren and Tree-sitter
// using different meanings for precedence, and bias
// it so they're mostly positive (aside from the really
// low precedence ones like a?b:c for which it makes
// sense).
function wren_to_ts_prec(wren_precedence) {
	return 10 - wren_precedence;
}

// See https://wren.io/syntax.html
const func_call_prec = wren_to_ts_prec(1);
const prefix_call_prec = wren_to_ts_prec(2);
const conditional_prec = wren_to_ts_prec(15);
const assignment_prec = wren_to_ts_prec(16);

// A utility function to make a comma-separated list from some given rule.
// This rule accepts an empty list of input rules.
// For example, comma_list_none(number) accepts all the following:
// <no input>
// 1
// 1,2
// 1,2,3
/// etc.
function comma_list_none(rule) {
	return optional(comma_list_one(rule));
}

// Same as comma_list, but requires at least one entry is present.
function comma_list_one(rule) {
	return seq(repeat(seq(rule, ',')), rule);
}

module.exports = grammar({
	name: 'wren',

	rules: {
		source_file: $ => seq(
			optional($.unix_interpreter),
			optional($._statement_sequence)
		),

		// This is the '#!' that can appear at the start of files to indicate
		// which executable should run them on UNIX.
		// See https://en.wikipedia.org/wiki/Shebang_(Unix)
		unix_interpreter: $ => /#!.*/,

		_statement: $ => choice(
			$.stmt_block,
			$.class_definition,
			$.stmt_break,
			$.stmt_continue,
			$.stmt_if,
			$.stmt_for,
			$.stmt_while,
			$.stmt_return,
			$.stmt_import,
			$.var_decl,
			$._expression,

			// FIXME: There's a bug in tree-sitter causing it to not recognise
			// rules only used in extras. Thus we have to use the comment
			// rule from somewhere in the main rules, otherwise block_comment
			// will cause a crash.
			// We need to make sure this rule will never trigger, since it was
			// causing the one-file test to fail.
			// https://github.com/tree-sitter/tree-sitter/issues/768
			seq('*** this should be impossible to match!', $.block_comment),
		),
		_statement_sequence: $ => seq(
			$._statement,
			repeat(seq(
				$._newline,
				optional($._statement),
			)),
		),

		stmt_block: $ => seq('{', optional($._statement_sequence), '}'),

		class_definition: $ => seq(
			repeat($._attribute),
			optional('foreign'),
			'class',
			field('name', $.identifier),
			optional(seq(
				'is',
				field('supertype', $._restricted_expression),
			)),
			'{',
			repeat($._class_item),
			'}',
		),

		stmt_break: $ => 'break',
		stmt_continue: $ => 'continue',

		stmt_if: $ => prec.right(seq(
			'if',
			'(',
			field('condition', $._expression),
			')',
			$._statement,
			optional(seq(
				'else',
				$._statement,
			)),
		)),

		stmt_for: $ => seq(
			'for',
			'(',
			$.identifier,
			'in',
			$._expression,
			')',
			$._statement,
		),

		stmt_while: $ => seq(
			'while',
			'(',
			$._expression,
			')',
			$._statement,
		),

		stmt_return: $ => seq(
			'return',
			optional($._expression),
		),

		stmt_import: $ => seq(
			'import',
			field('module', $.string_literal),
			optional(seq(
				'for',
				comma_list_one($._import_clause),
			)),
		),
		_import_clause: $ => seq(
			$.identifier,
			optional(seq(
				'as',
				field('as', $.identifier),
			)),
		),

		var_decl: $ => seq(
			'var',
			field('name', $.identifier),
			optional(seq(
				'=',
				$._expression,
			)),
		),

		// Used to resolve conflicts on the class import.
		// This isn't a distinction that exists in actual Wren, but it's
		// highly unlikely to ever matter.
		_restricted_expression: $ => choice(
			$.true_literal,
			$.false_literal,
			$.null_literal,
			$.string_literal,
			$.number,
			$.var_load,
			$.expr_brackets,
		),
		_expression: $ => choice(
			$._restricted_expression,
			$.function_call,
			$.subscript_call,
			$.this_call,
			$.infix_call,
			$.prefix_call,
			$.conditional,
			$.list_initialiser,
			$.map_initialiser,
			$.interpolated_string,
		),

		// This is just an identifier, but it's in it's own rule to make
		// processing ASTs easier.
		var_load: $ => $.identifier,

		true_literal: $ => 'true',
		false_literal: $ => 'false',
		null_literal: $ => 'null',

		expr_brackets: $ => prec(1, seq('(', $._expression, ')')),

		function_call: $ => {
			let rec_and_name = () => [field('receiver', $._expression), $._dot, field('name', $.identifier)];

			return prec(func_call_prec, choice(
			// Closure-creating call
			seq(...rec_and_name(), optional($._func_args), $.closure_block),

			// Regular function call
			seq(...rec_and_name(), optional($._func_args)),

			// Setter calls
			// Note that these still use the standard precedence, despite the
			// existence of the setter precedence. That's because (trying to
			// mimic Wren's parser) Wren's parser doesn't know whether or not
			// a function call is a setter until it's already selected it.
			prec.right(func_call_prec, seq(...rec_and_name(), '=', $._expression)),
		));},

		// Subscript calls (getter and setter)
		subscript_call: $ => prec.right(func_call_prec, seq(
			field('receiver', $._expression),
			$._subscript_args,
			optional(seq('=', field('assignment', $._expression)))
		)),

		// TODO deduplicate with function_call
		this_call: $ => choice(
			// Closure-creating call
			seq(field('name', $.identifier), optional($._func_args), $.closure_block),

			// Regular method call
			// Require function arguments here, to avoid ambiguity with an
			// identifier expression. That can only be found with symbol
			// lookups.
			seq(field('name', $.identifier), $._func_args),

			// Setter calls
			prec.right(seq(field('name', $.identifier), '=', $._expression)),
		),

		// The infix function calls - https://wren.io/syntax.html
		// To avoid a huge number of nearly-identical rules, we'll be
		// a bit hacky and use some imperative code to generate
		// the rules.
		infix_call: $ => {
			let choices = [];
			for (let operator of infix_operators) {
				for (var i=2; i<operator.length; i++) {
					let sym = operator[i];
					let associativity = operator[0];

					let precedence = wren_to_ts_prec(operator[1]);

					let rule = seq($._expression, sym, $._expression);

					if (associativity == 'left') {
						choices.push(prec.left(precedence, rule));
					} else {
						choices.push(prec.right(precedence, rule));
					}
				}
			}

			return choice(...choices);
		},
		prefix_call: $ => {
			let choices = [];
			for (let sym of prefix_operators) {
				let precedence = prefix_call_prec;

				let rule = seq(sym, $._expression);

				choices.push(prec.right(precedence, rule));
			}

			return choice(...choices);
		},
		_func_args: $ => seq('(', comma_list_none($._expression), ')'),
		_subscript_args: $ => seq('[', comma_list_one($._expression), ']'),
		closure_block: $ => seq('{', optional($.closure_params), optional($._statement_sequence), '}'),
		closure_params: $ => seq('|', comma_list_one($.identifier), '|'),

		conditional: $ => prec.right(conditional_prec, seq($._expression, '?', $._expression, ':', $._expression)),

		list_initialiser: $ => seq(
			'[',
			optional(seq(
				$._expression,
				repeat(seq(
					',', $._expression,
				)),
				// Allow trailing commas
				optional(','),
			)),
			']'
		),

		// Use a lower precedence to prefer stmt_block
		map_initialiser: $ => prec(-1, seq(
			'{',
			optional(seq(
				$._map_init_key,
				repeat(seq(
					',', $._map_init_key,
				)),
				// Allow trailing commas
				optional(','),
			)),
			'}'
		)),
		_map_init_key: $ => seq($._expression, ':', $._expression),

		// An interpolated string is made of a sequence of fragments (which are
		// strings that end with the start of an interpolation) followed by an
		// ending fragment.
		interpolated_string: $ => seq(
			$._string_start_fragment,
			repeat(seq(
				$._expression, $._string_mid_fragment,
			)),
			$._expression, $._string_end_fragment,
		),

		_class_item: $ => choice(
			$.method,
			$.foreign_method,
		),

		method: $ => seq(
			repeat($._attribute),
			$._method_sig,
			$.stmt_block,
		),
		foreign_method: $ => seq(
			repeat($._attribute),
			'foreign',
			$._method_sig,
		),
		_method_sig: $ => seq(
			optional('static'),
			optional('construct'),
			// Operators are valid names, as that's how infix and unary
			// operator functions are declared.
			choice(seq(
				field('name', choice($.identifier, $.operator_method_name)),
				optional($.param_list),
			), seq(
				$.subscript_param_list,
			)),
		),

		// Put this into it's own rule, so it's possible to find the name
		// of the method since there's no identifier.
		operator_method_name: $ => choice(...all_operators),

		param_list: $ => choice(
			seq('=', '(', $.identifier, ')'),
			seq('(', comma_list_none($.identifier), ')'),
		),
		subscript_param_list: $ => seq(
			'[', comma_list_one($.identifier), ']',
			optional(seq('=', '(', $.identifier, ')')),
		),

		_attribute: $ => choice($.attribute, $.runtime_attribute),
		attribute: $ => seq(
			'#',
			$.identifier,
			optional(choice(
				seq('=', $._attr_value),
				seq('(', comma_list_none($.group_item), ')'),
			)),
		),
		runtime_attribute: $ => seq(
			'#!',
			$.identifier,
			optional(choice(
				seq('=', $._attr_value),
				seq('(', comma_list_none($.group_item), ')'),
			)),
		),
		group_item: $ => seq($.identifier, optional(seq('=', $._attr_value))),
		_attr_value: $ => choice(
			$.true_literal,
			$.false_literal,
			$.string_literal,
			$.number,
			$.identifier,
		),

		comment: $ => /\/\/.*/, // Single-line comment
		block_comment: $ => seq('/*', repeat(choice($.block_comment, /[^/*]+/)), '*/'),

		identifier: $ => /[A-Za-z_][A-Za-z0-9_]*/,
		number: $ => choice(
			/-?[0-9]+(\.[0-9]+)?(e[+-]?[0-9]+)?/,
			/-?0x[0-9a-fA-F]+/,
		),
	},

	extras: $ => [
		$.comment,
		$.block_comment,
		$._newline,
		/\s/, // Whitespace
	],

	// I can't find a way to properly handle Wren's rule that
	// expression<newline>.func is valid, so parse the newlines and dot in C
	// so we can make them interact specially.
	// Also, handle strings there. Interpolation would be completely horrible
	// to implement with regexes.
	externals: $ => [
		$._dot,
		$._newline,
		$.string_literal,
		$._string_start_fragment, // " hello %(
		$._string_mid_fragment,   // ) many  %(
		$._string_end_fragment,   // ) worlds!"
	],

	word: $ => $.identifier,
});
