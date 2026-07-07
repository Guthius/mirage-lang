# Mirage Grammar (EBNF)

This grammar is derived directly from the parser in `src/compiler/ast.cpp`. Terminals are written in `'single quotes'` or as `UPPERCASE` for token classes. Optional parts are in `[brackets]`. Repetition is `{...}`. Alternatives are `|`.

---

## Top-Level Structure

```ebnf
program       ::= { declaration } EOF

declaration   ::= [ 'pub' ] fn_decl
               | [ 'pub' ] ext_fn_decl
               | [ 'pub' ] type_decl
               | [ 'pub' ] var_decl
               | [ 'pub' ] macro_decl
               | impl_decl           (* impl cannot be pub *)
```

---

## Declarations

### Function Declaration

```ebnf
fn_decl       ::= 'fn' IDENT '(' [ param { ',' param } ] ')' [ return_types ] stmt

param         ::= [ 'mut' ] IDENT ':' type

return_types  ::= '->' type                       (* single return *)
               | '->' '(' type { ',' type } ')'  (* multi-return *)
```

### Extern Function Declaration

```ebnf
ext_fn_decl   ::= ext_kw 'fn' IDENT '(' [ ext_params ] ')' [ '->' type ]

ext_kw        ::= 'ext'     (* parsed as identifier, not keyword *)

ext_params    ::= ext_param { ',' ext_param } [ ',' '...' ]
               | '...'      (* error: requires at least one named param *)

ext_param     ::= IDENT ':' type
```

### Type Declaration

```ebnf
type_decl     ::= 'type' IDENT '=' type
```

### Variable Declaration (top-level)

```ebnf
var_decl      ::= ( 'mut' | 'const' ) IDENT
                  ( ':' type [ '=' expr ]
                  | ':=' expr )
```

A `const` requires an initializer. A top-level `const` initializer may use `import(...)`.

### Macro Declaration

```ebnf
macro_decl    ::= 'macro' IDENT '(' [ macro_param { ',' macro_param } ] ')' '->' expr

macro_param   ::= IDENT ':' type
```

### Impl Block

```ebnf
impl_decl     ::= 'impl' named_type '{' { method_decl } '}'

method_decl   ::= [ 'pub' ] 'fn' IDENT
                  '(' ( 'self' | 'mut' 'self' )
                      { ',' IDENT ':' type }
                  ')'
                  [ return_types ]
                  stmt
```

---

## Types

```ebnf
type          ::= '*' type                              (* pointer *)
               | '[' ']' type                          (* slice *)
               | '[' expr ']' type                     (* array *)
               | 'struct' [ '(' 'packed' ')' ] '{'
                   { IDENT ':' type [ '=' expr ] }
                 '}'
               | 'enum' [ '(' type ')' ] '{'
                   { IDENT [ '=' expr ] }
                 '}'
               | 'union' '{'                            (* untagged union *)
                   { IDENT ':' type }
                 '}'
               | 'union' '(' 'enum' ')' '{'             (* tagged union *)
                   { IDENT [ ':' type ] }
                 '}'
               | fn_type
               | named_type
               | builtin_type

fn_type       ::= 'fn' '(' [ fn_type_params ] ')' [ '->' type | '->' '(' type { ',' type } ')' ]

fn_type_params ::= type { ',' type } [ ',' '...' ]
                | '...'

named_type    ::= IDENT { '.' IDENT }

builtin_type  ::= 'u8' | 'u16' | 'u32' | 'u64'
               | 'i8' | 'i16' | 'i32' | 'i64'
               | 'f32' | 'f64'
               | 'usize' | 'bool' | 'byte' | 'error' | 'anyptr'
```

Note: Struct and enum fields in type definitions are newline-separated (not comma-separated).

---

## Statements

```ebnf
stmt          ::= block_stmt
               | if_stmt
               | while_stmt
               | switch_stmt
               | var_decl_stmt
               | continue_stmt
               | break_stmt
               | return_stmt
               | defer_stmt
               | expr_stmt

block_stmt    ::= '{' { stmt } '}'

if_stmt       ::= 'if' expr stmt [ 'else' stmt ]

while_stmt    ::= 'while' expr block_stmt

switch_stmt   ::= 'switch' expr '{' [ switch_arm { ',' switch_arm } [ ',' ] ] '}'

switch_arm    ::= arm_pattern ':' stmt

continue_stmt ::= 'continue'

break_stmt    ::= 'break'

return_stmt   ::= 'return' [ expr { ',' expr } ]

defer_stmt    ::= 'defer' stmt

expr_stmt     ::= expr

var_decl_stmt ::= ( 'mut' | 'const' ) IDENT
                  ( ':' type [ '=' expr ]               (* typed, optional init *)
                  | ',' IDENT { ',' IDENT } ':=' expr   (* group decl *)
                  | ':=' expr )                         (* inferred type *)
```

For group declarations, any name position may be `_` (written as an identifier) to discard that return value:
```
mut val, _ := call()
```

---

## Expressions

Expressions are parsed via precedence climbing. Listed from lowest to highest precedence:

```ebnf
expr          ::= assign_expr
               | import_expr          (* only in const := position *)

import_expr   ::= 'import' '(' STRING ')'

assign_expr   ::= ternary_expr [ assign_op assign_expr ]

assign_op     ::= '=' | '+=' | '-=' | '*=' | '/='
               | '&=' | '|=' | '^=' | '<<=' | '>>='

ternary_expr  ::= logical_or_expr [ '?' expr ':' expr ]

logical_or_expr  ::= logical_and_expr { '||' logical_and_expr }

logical_and_expr ::= bitwise_or_expr  { '&&' bitwise_or_expr }

bitwise_or_expr  ::= bitwise_xor_expr { '|'  bitwise_xor_expr }

bitwise_xor_expr ::= bitwise_and_expr { '^'  bitwise_and_expr }

bitwise_and_expr ::= equality_expr    { '&'  equality_expr }

equality_expr    ::= comparison_expr  { ( '==' | '!=' ) comparison_expr }

comparison_expr  ::= shift_expr  { ( '<' | '>' | '<=' | '>=' ) shift_expr }

shift_expr    ::= additive_expr { ( '<<' | '>>' ) additive_expr }

additive_expr ::= mult_expr  { ( '+' | '-' ) mult_expr }

mult_expr     ::= unary_expr { ( '*' | '/' | '%' ) unary_expr }

unary_expr    ::= try_expr
               | ( '-' | '!' | '~' | '&' | '*' ) unary_expr
               | postfix_expr

try_expr      ::= 'try' postfix_expr

postfix_expr  ::= primary_expr { postfix_op }

postfix_op    ::= '(' [ arg { ',' arg } ] ')'    (* call *)
               | '.' IDENT                        (* member access *)
               | '.' IDENT '{.' field_init { ',' field_init } '}'  (* qualified tagged variant constructor *)
               | '[' expr ']'                     (* index *)
               | '[' expr '..' expr ']'           (* slice *)
               | '++'
               | '--'

arg           ::= expr
```

### Primary Expressions

```ebnf
primary_expr  ::= INT_LITERAL
               | FLOAT_LITERAL
               | STRING_LITERAL
               | 'true'
               | 'false'
               | 'nil'
               | IDENT
               | 'iota'
               | 'default'
               | 'undefined'
               | '(' expr ')'
               | sizeof_expr
               | len_expr
               | cast_expr
               | match_expr
               | braced_initializer
               | dot_ident_expr
               | contextual_tagged_variant

sizeof_expr   ::= 'sizeof' expr

len_expr      ::= 'len' expr

cast_expr     ::= 'cast' '(' expr ',' type [ ',' expr ] ')'

match_expr    ::= 'match' expr '{' [ match_arm { ',' match_arm } [ ',' ] ] '}'

match_arm     ::= arm_pattern ':' expr

arm_pattern   ::= '.' IDENT [ '(' [ '&' ] IDENT ')' ]    (* variant pattern *)
               | '_'                                        (* default/wildcard *)
               | expr                                       (* literal pattern, must be constant *)

dot_ident_expr ::= '.' IDENT   (* enum literal; valid where enum type is expected *)

contextual_tagged_variant ::= '.' IDENT '{.' field_init { ',' field_init } '}'

field_init    ::= '.' IDENT '=' expr

braced_initializer ::= '{' '}'                                    (* empty *)
               | '{' IDENT '=' expr { ',' IDENT '=' expr } '}'   (* struct fields *)
               | '{' expr { ',' expr } [ '...' ] '}'              (* array values, optional trailing fill *)
```

---

## Literals

```ebnf
INT_LITERAL   ::= decimal_int | hex_int | bin_int

decimal_int   ::= DIGIT { DIGIT | '_' }
hex_int       ::= '0x' HEX_DIGIT { HEX_DIGIT | '_' }
bin_int       ::= '0b' BIN_DIGIT { BIN_DIGIT | '_' }

FLOAT_LITERAL ::= DIGIT { DIGIT } '.' DIGIT { DIGIT }

STRING_LITERAL ::= '"' { char | escape_seq } '"'

escape_seq    ::= '\\' | '\"' | '\n' | '\t' | '\r' | '\0'

IDENT         ::= LETTER { LETTER | DIGIT | '_' }

DIGIT         ::= '0'..'9'
HEX_DIGIT     ::= '0'..'9' | 'a'..'f' | 'A'..'F'
BIN_DIGIT     ::= '0' | '1'
LETTER        ::= 'a'..'z' | 'A'..'Z' | '_'
```

---

## Notes on Syntax Conventions

1. **Struct field separators**: Struct fields in type definitions use newlines as separators (not commas). In `StructExpr` (braced init), fields use `.name = val` syntax with commas: `{.x = 1, .y = 2}`.

2. **Match/switch arm separator**: Arms are comma-separated; a trailing comma before `}` is optional.

3. **Multi-return**: Return values are comma-separated: `return a, b`. Multi-return type annotations use parentheses: `-> (T1, T2)`.

4. **`try` precedence**: `try` binds tighter than binary operators: `try f(x) + g()` parses as `(try f(x)) + g()`. To chain member access after try, use parentheses: `(try f(x)).field`.

5. **Braced initializer disambiguation**: A `{` followed by `.identifier` signals a struct or tagged union field initializer. `{` followed by anything else is an array initializer, empty initializer `{}`, or a block statement.

6. **`ext` keyword**: `ext` is scanned as an `IDENT` with the lexeme `"ext"`, not as a keyword token. It is valid only at the start of a declaration in `ext fn name(...)` form.

7. **`_` in group declarations**: The underscore `_` is recognized as an `IDENT` with value `"_"` in group declarations and match default patterns; it is not a keyword.

8. **Variadic `...` in `fn` type**: Inside a function-pointer type expression, `...` marks the function as variadic and must be the last parameter.

9. **`for` keyword**: Reserved but not yet implemented as a statement form.

10. **Array fill `...` in array initializer**: In an array initializer `{ expr, ... }`, a trailing `...` immediately after the last value repeats that value (evaluated once) to fill all remaining elements of the array. It must be the last token before `}`.
