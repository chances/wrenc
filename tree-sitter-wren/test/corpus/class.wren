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
