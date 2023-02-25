==============
Method closures
==============

Fn.new {
    System.print("hello world")
}

Fn.new(1) {
    System.print("hello world")
}

-----

(source_file
    (function_call receiver: (identifier) name: (identifier)
        (stmt_block
            (function_call receiver: (identifier) name: (identifier) (string_literal))
        )
    )

    (function_call receiver: (identifier) name: (identifier) (number)
        (stmt_block
            (function_call receiver: (identifier) name: (identifier) (string_literal))
        )
    )
)

==============
Constructors
==============

class Cls {
    construct my_ctor {}
    construct my_ctor() {}
    construct my_ctor(a) {}
    construct my_ctor(a, b, c) {}
}

-----

(source_file
    (class_definition name: (identifier)
        (method name: (identifier) (stmt_block))
        (method name: (identifier) (param_list) (stmt_block))
        (method name: (identifier) (param_list (identifier)) (stmt_block))
        (method name: (identifier) (param_list (identifier) (identifier) (identifier)) (stmt_block))
    )
)

==============
Static methods
==============

class Cls {
    static my_static {}
    static my_static() {}
    static my_static=(a) {}
}

-----

(source_file
    (class_definition name: (identifier)
        (method name: (identifier) (stmt_block))
        (method name: (identifier) (param_list) (stmt_block))
        (method name: (identifier) (param_list (identifier)) (stmt_block))
    )
)

==============
Foreign methods
==============

class Cls {
    foreign my_func
    foreign my_func()
    foreign my_func(a, b, c)
    foreign my_func=(a)

    foreign construct new
    foreign static my_func
}

-----

(source_file
    (class_definition name: (identifier)
        (foreign_method name: (identifier))
        (foreign_method name: (identifier) (param_list))
        (foreign_method name: (identifier) (param_list (identifier) (identifier) (identifier)))
        (foreign_method name: (identifier) (param_list (identifier)))

        (foreign_method name: (identifier))
        (foreign_method name: (identifier))
    )
)

==============
This dispatch
==============

class Cls {
    method {
        a // Treated as an identifier, not a method call
        a()
        a(1)
        a(1,2,3)
        a = 123

        a {}
        a(1,2,3) {}

        a
        {}
    }
}

-----

(source_file
    (class_definition name: (identifier)
        (method name: (identifier) (stmt_block
            (identifier) (comment)
            (this_call name: (identifier))
            (this_call name: (identifier) (number))
            (this_call name: (identifier) (number) (number) (number))
            (this_call name: (identifier) (number))

            (this_call name: (identifier) (stmt_block))
            (this_call name: (identifier) (number) (number) (number) (stmt_block))

            (identifier)
            (stmt_block)
        ))
    )
)

==============
Foreign class
==============

foreign class Cls {}

-----

(source_file
    (class_definition name: (identifier))
)

==============
Subscript methods
==============

class Cls {
    func[a] {}
    func[a, b] {}
    func[a, b, c] {}

    func[a]=(v) {}
    func[a, b]=(v) {}
    func[a, b, c]=(v) {}
}

-----

(source_file
    (class_definition name: (identifier)
        (method name: (identifier) (param_list (identifier)) (stmt_block))
        (method name: (identifier) (param_list (identifier) (identifier)) (stmt_block))
        (method name: (identifier) (param_list (identifier) (identifier) (identifier)) (stmt_block))

        (method name: (identifier) (param_list (identifier) (identifier)) (stmt_block))
        (method name: (identifier) (param_list (identifier) (identifier) (identifier)) (stmt_block))
        (method name: (identifier) (param_list (identifier) (identifier) (identifier) (identifier)) (stmt_block))
    )
)

==============
Subscript calls
==============

abc[1]
abc[1, 2]
abc[1, 2, 3]
a.abc[1, 2, 3]

abc[1] = 123
abc[1, 2] = 123
abc[1, 2, 3] = 123
a.abc[1, 2, 3] = 123

-----

(source_file
    (this_call name: (identifier) (number))
    (this_call name: (identifier) (number) (number))
    (this_call name: (identifier) (number) (number) (number))
    (function_call receiver: (identifier) name: (identifier) (number) (number) (number))

    (this_call name: (identifier) (number) (number))
    (this_call name: (identifier) (number) (number) (number))
    (this_call name: (identifier) (number) (number) (number) (number))
    (function_call receiver: (identifier) name: (identifier) (number) (number) (number) (number))
)

==============
Mixed infix and unary calls
==============

-a + 3
!a * 3
~a && 3

a + -b

-----

(source_file
    (infix_call (prefix_call (identifier)) (number))
    (infix_call (prefix_call (identifier)) (number))
    (infix_call (prefix_call (identifier)) (number))

    (infix_call (identifier) (prefix_call (identifier)))
)

==============
Infix function definitions
==============

class A {
    -(other) {}
    /(other) {}
    &&(other) {}
}

-----

(source_file (class_definition (identifier)
    (method (operator_method_name) (param_list (identifier)) (stmt_block))
    (method (operator_method_name) (param_list (identifier)) (stmt_block))
    (method (operator_method_name) (param_list (identifier)) (stmt_block))
))

==============
Prefix function definitions
==============

class A {
    - {}
    ! {}
    ~ {}
    foreign !
    abc {}
}

-----

(source_file (class_definition (identifier)
    (method (operator_method_name) (stmt_block))
    (method (operator_method_name) (stmt_block))
    (method (operator_method_name) (stmt_block))
    (foreign_method (operator_method_name))
    (method (identifier) (stmt_block))
))

==============
Superclass extends
==============

// This is where one class extends another - the test name isn't great

class A is B {}
class A is (B.hello) {}
class A is ("hello, world!") {}

-----

(source_file
    (comment)
    (class_definition name: (identifier) supertype: (identifier))
    (class_definition name: (identifier) supertype: (expr_brackets (function_call receiver: (identifier) name: (identifier))))
    (class_definition name: (identifier) supertype: (expr_brackets (string_literal)))
)
