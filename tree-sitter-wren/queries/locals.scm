(method) @local.scope
(stmt_block) @local.scope
(closure_block) @local.scope

(var_decl name: (identifier) @local.definition)
(class_definition name: (identifier) @local.definition)
; TODO: Add selectors for private class members, getters, and setters. Nova can specifically outline these members.
; Methods
(method name: (identifier) @local.definition)
(method name: (operator_method_name) @local.definition)
(foreign_method name: (identifier) @local.definition)
(foreign_method name: (operator_method_name) @local.definition)
; Function Parameters
(method (param_list (identifier) @local.definition))
; Closure Parameters
(closure_block (closure_params (identifier) @local.definition))

(identifier) @local.reference
