==============
For loop syntax
==============

// From Wren's test/language/for/syntax.wren test.

// Single-expression body.
for (i in [1]) System.print(i)

// Block body.
for (i in [1]) {
  System.print(i)
}

// Newline after "in".
for (i in
  [1]) System.print(i)

-----

(source_file
    (comment)

    (comment)
    (stmt_for
        (identifier)
        (list_initialiser (number))
        (function_call receiver: (var_load (identifier)) name: (identifier) (var_load (identifier)))
    )

    (comment)
    (stmt_for
        (identifier)
        (list_initialiser (number))
        (stmt_block
            (function_call receiver: (var_load (identifier)) name: (identifier) (var_load (identifier)))
        )
    )

    (comment)
    (stmt_for
        (identifier)
        (list_initialiser (number))
        (function_call receiver: (var_load (identifier)) name: (identifier) (var_load (identifier)))
    )
)
