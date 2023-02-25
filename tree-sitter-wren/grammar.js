module.exports = grammar({
	name: 'wren',

	rules: {
		// TODO: add the actual grammar rules
		source_file: $ => optional($._statement_sequence),

		_statement: $ => choice(
			$.stmt_block,
			$.class_definition,
			$.stmt_break,
			$.stmt_continue,
			$.stmt_if,
			$.stmt_for,
			$.stmt_return,
			$.stmt_import,
			$.var_decl,
			$._expression,

			// FIXME There's a bug in tree-sitter causing it to not recognise
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

		stmt_if: $ => seq(
			'if',
			'(',
			field('condition', $._expression),
			')',
			$._statement,
		),

		stmt_for: $ => seq(
			'for',
			'(',
			$.identifier,
			'in',
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
				$._import_clause,
				repeat(seq(',', $._import_clause)),
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
			$.identifier,
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
			$.identifier,
			$.expr_brackets,
		),
		_expression: $ => choice(
			$._restricted_expression,
			$.function_call,
			$.this_call,
			$.infix_call,
			$.list_initialiser,
			$.map_initialiser,
		),

		true_literal: $ => 'true',
		false_literal: $ => 'false',
		null_literal: $ => 'null',

		// TODO interpolation, escapes
		string_literal: $ => /"[^"]*"/,

		expr_brackets: $ => prec(1, seq('(', $._expression, ')')),

		function_call: $ => choice(
			// Closure-creating call
			seq(field('receiver', $._expression), '.', field('name', $.identifier), optional($._func_args),
				$.stmt_block,
			),

			// Regular function call
			seq(field('receiver', $._expression), '.', field('name', $.identifier), optional($._func_args)),

			// Setter calls
			prec.right(seq(field('receiver', $._expression), '.', field('name', $.identifier), '=', $._expression)),

			// Subscript calls (getter and setter)
			prec.right(seq(field('receiver', $._expression), '.', field('name', $.identifier), $._subscript_args, optional(seq('=', $._expression)))),
		),

		// TODO deduplicate with function_call
		this_call: $ => choice(
			// Closure-creating call
			seq(field('name', $.identifier), optional($._func_args), $.stmt_block),

			// Regular method call
			// Require function arguments here, to avoid ambiguity with an
			// identifier expression. That can only be found with symbol
			// lookups.
			seq(field('name', $.identifier), $._func_args),

			// Setter calls
			prec.right(seq(field('name', $.identifier), '=', $._expression)),

			// Subscript calls (getter and setter)
			prec.right(seq(field('name', $.identifier), $._subscript_args, optional(seq('=', $._expression)))),
		),

		// The infix function calls - https://wren.io/syntax.html
		// To avoid a huge number of nearly-identical rules, we'll be
		// a bit hacky and use some imperitive code to generate
		// the rules.
		infix_call: $ => {
			let operators = [
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

			let choices = [];
			for (let operator of operators) {
				for (var i=2; i<operator.length; i++) {
					let sym = operator[i];
					let associativity = operator[0];
					let precidence = operator[1];

					let rule = seq($._expression, sym, $._expression);

					if (associativity == 'left') {
						choices.push(prec.left(3, rule));
					} else {
						choices.push(prec.right(3, rule));
					}
				}
			}

			return choice(...choices);
		},
		_func_args: $ => choice(
			seq('(', ')'),
			seq('(', $._expression, ')'),
			seq('(', $._expression, repeat(seq(',', $._expression)), ')'),
		),
		_subscript_args: $ => choice(
			seq('[', $._expression, ']'),
			seq('[', $._expression, repeat(seq(',', $._expression)), ']'),
		),

		list_initialiser: $ => seq(
			'[',
			optional(seq(
				$._expression,
				repeat(seq(
					',', $._expression,
				)),
			)),
			']'
		),

		// Use a lower precidence to prefer stmt_block
		map_initialiser: $ => prec(-1, seq(
			'{',
			optional(seq(
				$._map_init_key,
				repeat(seq(
					',', $._map_init_key,
				)),
			)),
			'}'
		)),
		_map_init_key: $ => seq($._expression, ':', $._expression),

		_class_item: $ => choice(
			$.method,
			$.foreign_method,
		),

		method: $ => seq(
			optional('static'),
			optional('construct'),
			field('name', $.identifier),
			optional($.param_list),
			$.stmt_block,
		),

		foreign_method: $ => seq(
			'foreign',
			optional('static'),
			optional('construct'),
			field('name', $.identifier),
			optional($.param_list),
		),

		param_list: $ => choice(
			seq('=', '(', $.identifier, ')'),
			seq('(', ')'),
			seq('(', $.identifier, repeat(seq(',', $.identifier)), ')'),
			seq('[', $.identifier, repeat(seq(',', $.identifier)), ']', optional(seq('=', '(', $.identifier, ')'))),
		),

		comment: $ => /\/\/.*/, // Single-line comment
		block_comment: $ => seq('/*', repeat(choice($.block_comment, /[^/*]+/)), '*/'),

		_newline: $ => '\n',
		identifier: $ => /[A-Za-z_][A-Za-z0-9_]*/,
		number: $ => choice(
			/-?[0-9]+(\.[0-9]+)?(e[+-]?[0-9]+)?/,
			/0x[0-9a-fA-F]+/,
		),
	},

	extras: $ => [
		$.comment,
		$.block_comment,
		$._newline,
		/\s/, // Whitespace
	],

	word: $ => $.identifier,
});
