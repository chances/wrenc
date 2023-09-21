; Keywords
"as" @keyword
(stmt_break) @keyword
"class" @keyword
"construct" @keyword
(stmt_continue) @keyword
"else" @keyword
(false_literal) @keyword.bool
"for" @keyword
"foreign" @keyword
"if" @keyword
"import" @keyword
"in" @keyword
"is" @keyword
(null_literal) @keyword.null
"return" @keyword
"static" @keyword
; FIXME: This might highlight _all_ identifiers as keywords
((identifier) @keyword (#eq? @keyword "super"))
((identifier) @keyword (#eq? @keyword "this"))
(true_literal) @keyword.bool
"var" @keyword
"while" @keyword

; Comments
(comment) @comment
(block_comment) @comment

; Literals
(number) @number
(string_literal) @string
; String Interpolations
(interpolated_string) @string
; TODO: Inject the `_expression` context into interpolated values

; Attributes
(attribute) @attribute
(runtime_attribute) @attribute.runtime

; Declarations
(var_decl name: (identifier) @variable)

; Closures
(closure_block) @function.closure

; Classes
(class_definition name: (identifier) @class)

; TODO: Add selectors for private class members, getters, and setters.
; Nova (and other editors?) can specifically outline these members.

; Methods
(method name: (identifier) @method)
(method (param_list (identifier) @variable.parameter))
; FIXME: (method name: (identifier) (param_list) @method.setter)
(method name: (operator_method_name) @method.operator)
(foreign_method name: (identifier) @method)
(foreign_method (param_list (identifier) @variable.parameter))
; FIXME: (foreign_method name: (identifier) (param_list) @method.setter)
(foreign_method name: (operator_method_name) @method.operator)

; TODO: Add selectors for `static` methods and constructors.
; Nova (and other editors?) can specifically outline these members.
