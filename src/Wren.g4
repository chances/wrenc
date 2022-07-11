grammar Wren;

// The reserved words
TOK_break: 'break';
TOK_class: 'class';
TOK_construct: 'construct';
TOK_else: 'else';
TOK_false: 'false';
TOK_for: 'for';
TOK_foreign: 'foreign';
TOK_if: 'if';
TOK_import: 'import';
TOK_in: 'in';
TOK_is: 'is';
TOK_null: 'null';
TOK_return: 'return';
TOK_static: 'static';
TOK_super: 'super';
TOK_this: 'this';
TOK_true: 'true';
TOK_var: 'var';
TOK_while: 'while';

// TEMPORARY
// For now, use semicolons - we'll add support for Wren's clever and simple newline rules later
WS : (' ' | '\t' | '\n')+ -> skip;
EOL: ';';
ID : [a-zA-Z][a-zA-Z_0-9]*;
FIELD : '_' [a-zA-Z_0-9]*;
STRING_LIT: '"' .*? '"';


root: stmt*;

stmt:
    EOL # EmptyStmt
  | expr EOL # ExprStmt
  | TOK_class name=ID '{' class_member* '}' EOL # ClassDef
  | block # BlockStmt
;

expr:
    func=expr '(' ((expr ',')* expr)? ')' # FuncCall
  | expr '.' ID # MethodRef
  | FIELD # FieldRef // AFAIK fields CANNOT be used with the 'this.' syntax
  | ID # VariableRef
  | STRING_LIT # StringLit
;

block: '{' stmt* '}';

class_member:
    name=ID '(' ((arg+=ID ',')* arg+=ID)? ')' block # FuncDef
;

