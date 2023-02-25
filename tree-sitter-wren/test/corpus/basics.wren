==============
Large basic test
==============

// Single-line comments.

class A {
    my_method(arg, a, b, c) {
        if (arg == 123) {
            return null
        }
        return arg + 1
    }

    setter=(value) {
        this.abc = 123
        System.print(1234, value.hello)
        System.print(1, 2, 3, 4, 5)
    }
}

-----

(source_file
    (comment)
    (class_definition name: (identifier)
        (method name: (identifier)
            (param_list
                (identifier) (identifier) (identifier) (identifier)
            )
            (stmt_block
                (stmt_if
                    condition: (infix_call (identifier) (number))
                    (stmt_block (stmt_return (null_literal)))
                )
                (stmt_return (infix_call (identifier) (number)))
            )
        )
        (method name: (identifier)
            (param_list (identifier))
            (stmt_block
                (function_call receiver: (identifier) name: (identifier) (number))
                (function_call receiver: (identifier) name: (identifier) (number) (function_call receiver: (identifier) name: (identifier)))
                (function_call receiver: (identifier) name: (identifier) (number) (number) (number) (number) (number))
            )
        )
    )
)

==============
Block end-of-line matching
==============

if (true) {
    return null
}

-----

(source_file
    (stmt_if (true_literal) (stmt_block
        (stmt_return (null_literal))
    ))
)

==============
Comments
==============

// This is a comment
// This is also a comment

/* This is a
m
u
l
t
i
-line comment.
*/

-----

(source_file
    (comment)
    (comment)
    (block_comment)
)

==============
Nested multi-line comments
==============

/* Outer

outer /* inner */ outer

End of outer */

-----

(source_file
    (block_comment (block_comment))
)

==============
Empty
==============

-----

(source_file)

==============
Only one comment
==============

// Hello

-----

(source_file (comment))

==============
Empty blocks
==============

{}

{ }

-----

(source_file (stmt_block) (stmt_block))

==============
Binary operators
==============

1 + 2
1 / 2
1 .. 2
1 << 2
1 & 2
1 ^ 2
1 | 2
1 < 2
1 is 2
1 == 2
1 && 2
1 || 2

-----

(source_file
    (infix_call (number) (number))
    (infix_call (number) (number))
    (infix_call (number) (number))
    (infix_call (number) (number))
    (infix_call (number) (number))
    (infix_call (number) (number))
    (infix_call (number) (number))
    (infix_call (number) (number))
    (infix_call (number) (number))
    (infix_call (number) (number))
    (infix_call (number) (number))
    (infix_call (number) (number))
)

==============
Precidence
==============

a * b - c
a - b * c

-----

(source_file
    (infix_call (infix_call (identifier) (identifier)) (identifier))
    (infix_call (identifier) (infix_call (identifier) (identifier)))
)

==============
Imports
==============

// These go here because there's not really a better place to put them

import "my_module"
import "my_module" for A
import "my_module" for A, B
import "my_module" for A, B, C
import "my_module" for A as a, B, C as c

-----

(source_file
    (comment)
    (stmt_import module: (string_literal))
    (stmt_import module: (string_literal) (identifier))
    (stmt_import module: (string_literal) (identifier) (identifier))
    (stmt_import module: (string_literal) (identifier) (identifier) (identifier))
    (stmt_import module: (string_literal)
        (identifier) as: (identifier)
        (identifier)
        (identifier) as: (identifier)
    )
)
