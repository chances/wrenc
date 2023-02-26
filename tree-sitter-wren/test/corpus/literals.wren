==============
String literals
==============

"hello"

-----

(source_file (string_literal))

==============
String escapes
==============

"hello \" \n \\ "

-----

(source_file (string_literal))

==============
Raw string literals
==============

a = """
hello world!
"""

"""
a string containing "quotes" and "" <= doubled-up quotes.

What about \""" three quotes but with an escape on the first one?

At the end of the line: ""
"""

-----

(source_file
    (this_call (identifier) (raw_string_literal))
    (raw_string_literal)
)

==============
Number literals
==============

1
123
123.456

-123
-123.456

123e1
0.1e1
10e-1
0.1e+1

0xdeadbeef
-0xdeadbeef

-----

(source_file
    (number) (number) (number)
    (number) (number)
    (number) (number) (number) (number)
    (number) (number)
)

==============
List literals
==============

[]
[1]
[1,2]
[1,2,3]
[1,[2],3]

// Trailing commas
[1,]
[1,2,]
[1,2,3,]

-----

(source_file
    (list_initialiser)
    (list_initialiser (number))
    (list_initialiser (number) (number))
    (list_initialiser (number) (number) (number))
    (list_initialiser (number) (list_initialiser (number)) (number))

    (comment)
    (list_initialiser (number))
    (list_initialiser (number) (number))
    (list_initialiser (number) (number) (number))
)

==============
Map literals
==============

a = {}
a = {1:1}
a = {1:1, 2:2}
a = {1:1, 2:2, 3:3}
a = {1:1, 2:{2:"inner!"}, 3:3}

{}

// Trailing commas
a = {1:2,}
a = {1:2,3:4,}

-----

(source_file
    (this_call name: (identifier) (map_initialiser))
    (this_call name: (identifier) (map_initialiser (number) (number)))
    (this_call name: (identifier) (map_initialiser (number) (number) (number) (number)))
    (this_call name: (identifier) (map_initialiser (number) (number) (number) (number) (number) (number)))
    (this_call name: (identifier) (map_initialiser
        (number) (number)
        (number) (map_initialiser (number) (string_literal))
        (number) (number))
    )
    (stmt_block)
    (comment)
    (this_call name: (identifier) (map_initialiser (number) (number)))
    (this_call name: (identifier) (map_initialiser (number) (number) (number) (number)))
)
