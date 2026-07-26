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
               | link_decl           (* cannot be pub *)
               | diagnostic_decl     (* cannot be pub *)
               | when_decl           (* cannot be pub *)
               | asm_stmt            (* cannot be pub; always a SEMA error at module scope —
                                         parses here (like link_decl/diagnostic_decl above)
                                         purely so the diagnostic can name it precisely *)
```

---

## Declarations

### Function Declaration

```ebnf
fn_decl       ::= 'fn' IDENT '(' [ param { ',' param } ] ')' [ return_types ] stmt

param         ::= [ 'mut' ] IDENT ':' type [ '=' expr ]  (* typed, optional default *)
               | [ 'mut' ] IDENT ':=' expr               (* inferred type, default required *)
               | IDENT ':' '...' type                    (* native variadic; must be the last parameter *)

return_types  ::= '->' type                       (* single return *)
               | '->' '(' type { ',' type } ')'  (* multi-return *)
```

A parameter's `':=' expr` form infers the parameter's type from the default
expression's type (same literal-defaulting rules as `var_decl_stmt`'s `:=`
form). Once any parameter in a list has a default value, every parameter
after it must also have one — `self` is exempt (never passed explicitly at
call sites, not subject to this rule). A native variadic parameter (`'...'
type`) cannot appear in the same parameter list as any defaulted parameter.
Default expressions are checked in module scope, not the function's own
local scope — they may reference module-scope symbols (consts, other
functions, imported modules) but never another parameter of the same
function, and may not contain `try`. See spec.md's "Default Parameter
Values" section for the full semantics.

### Extern Function Declaration

```ebnf
ext_fn_decl   ::= ext_kw 'fn' IDENT '(' [ ext_params ] ')' [ '->' type ]

ext_kw        ::= 'ext'     (* parsed as identifier, not keyword *)

ext_params    ::= ext_param { ',' ext_param } [ ',' '...' ]
               | '...'      (* error: requires at least one named param *)

ext_param     ::= IDENT ':' type [ '=' expr ]
```

The `[ '=' expr ]` on `ext_param` is accepted by the parser only so sema can
reject it with a clear diagnostic ("`ext fn` declarations may not have
default parameter values") rather than a generic parse error — it is never
valid on an `ext fn` declaration. There is no `':='` inferred-type form for
`ext_param`; `ext fn` parameters always require an explicit type.

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
macro_decl    ::= 'macro' IDENT '(' [ macro_param { ',' macro_param } ] ')' [ ':' type ] '->' expr

macro_param   ::= IDENT ':' type
```

### Impl Block

```ebnf
impl_decl     ::= 'impl' named_type '{' { method_decl } '}'                    (* bare impl *)
               | 'impl' named_type 'for' named_type '{' { method_decl } '}'  (* trait impl *)

method_decl   ::= [ 'pub' ] 'fn' IDENT
                  '(' ( 'self' | 'mut' 'self' )
                      { ',' ( [ 'mut' ] IDENT ':' type [ '=' expr ]
                            | [ 'mut' ] IDENT ':=' expr ) }
                  ')'
                  [ return_types ]
                  stmt
```

`method_decl`'s non-self params accept the same default-value forms as
`param` above (see the note after `fn_decl`'s grammar). In `impl TRAIT for
TYPE { ... }`, a method whose corresponding trait method declares a default
must be declared here *without* that default (it's inherited); declaring a
default the trait method doesn't have, or redeclaring one it does, is a
sema error.

In the trait-impl form (`impl TRAIT for TYPE { ... }`), `pub` on an individual
`method_decl` is rejected — the trait's own visibility governs which methods
are externally callable, not the impl block. See spec.md's "Traits and
Dynamic Dispatch" section for the full semantics.

Note: `method_decl`'s non-self params accept an optional `mut` prefix in the
implementation (this differs from `trait_method_decl`'s params below, which do
not accept `mut` at all).

### Link Declaration

```ebnf
link_decl     ::= '@link' '(' link_category ',' expr ')'

link_category ::= 'lib' | 'system' | 'flag'
```

`@` is a sigil, not part of an identifier; `link` (like `ext` above) is parsed
as a plain identifier immediately following it, not a reserved keyword. A
module-scope linker directive: `lib` links a library file (path relative to
the directory of the current module file), `system` links a system library by
name, `flag` passes a raw linker flag verbatim. `data` must be a compile-time
constant `[]u8` expression — the `when` expression form is legal here (see
below). Legal at module scope or inside a module-scope `when` block; legal
nowhere else (a sema error, not a parse error — see spec.md).

### Diagnostic Declaration

```ebnf
diagnostic_decl ::= diagnostic_kw '(' expr ')'

diagnostic_kw   ::= 'error' | 'warn'    (* '@error'/'@warn'; 'error' is the KwError
                                            keyword, 'warn' a plain identifier *)
```

`@` is a sigil, not part of the keyword/identifier that follows it. A
compile-time diagnostic directive: `@error` emits a sema error, `@warn` a
sema warning, at the directive's location. `expr` must be a compile-time
constant `[]u8` expression, exactly like `link_decl`'s `data` above. Legal
at module scope or inside a module-scope `when` block; legal nowhere else
(a sema error, not a parse error — see spec.md), mirroring `link_decl`
exactly.

### When Declaration

```ebnf
when_decl     ::= 'when' expr block_decl [ 'else' ( when_decl | block_decl ) ]

block_decl    ::= '{' { declaration } '}'
```

A compile-time conditional declaration block. `expr` must be a compile-time
constant expression. Parses any declaration kind inside `block_decl`; sema
restricts the permitted kinds to `@link`, `@error`, `@warn`, `const` with
`@option`/`@env`, `type`, and `ext fn` (see spec.md's "Compile-Time
Configuration" section).

---

## Types

```ebnf
type          ::= '*' type                              (* pointer *)
               | '[' ']' type                          (* slice *)
               | '[' expr ']' type                     (* array *)
               | '[' '?' ']' type                       (* array, size inferred from initializer *)
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
               | 'trait' '{' { trait_method_decl } '}'  (* trait handle; see Traits below *)
               | error_type
               | bitset_type
               | fn_type
               | named_type
               | builtin_type

error_type    ::= 'error' '(' named_type { '|' named_type } ')'

bitset_type   ::= 'bitset' '(' named_type [ ',' builtin_type ] ')'

fn_type       ::= 'fn' '(' [ fn_type_params ] ')' [ '->' type | '->' '(' type { ',' type } ')' ]

fn_type_params ::= fn_type_param { ',' fn_type_param } [ ',' '...' ]
                | '...'

fn_type_param ::= [ IDENT ':' ] type

named_type    ::= IDENT { '.' IDENT }

builtin_type  ::= 'u8' | 'u16' | 'u32' | 'u64'
               | 'i8' | 'i16' | 'i32' | 'i64'
               | 'f32' | 'f64'
               | 'usize' | 'bool' | 'byte' | 'anyptr'

trait_method_decl ::= 'fn' IDENT
                      '(' ( 'self' | 'mut' 'self' )
                          { ',' ( IDENT ':' type [ '=' expr ] | IDENT ':=' expr ) }
                      ')'
                      [ return_types ]
```

A trait must declare at least one method (an empty `trait { }` is a parse
error). `trait_method_decl` is signature-only (no body — a body is a parse
error) and does not accept `pub` (a parse error — the trait's own visibility
governs) or a native-variadic (`...T`) parameter (a parse error — variadic
trait methods have no vtable entry representation). Unlike `method_decl`
above, `trait_method_decl`'s non-self params do **not** accept a `mut`
prefix.

A trait method may declare default parameter values, following the same
rules as `fn_decl`'s `param` above. When a trait handle (`dyn`-style dynamic
dispatch) call omits an argument, the default always comes from the trait's
own declaration — never from whichever concrete impl happens to be behind
the handle — and is resolved before the vtable call.

Note: Struct, enum, and union fields/variants need no separator token between them at all (see Notes on Syntax Conventions, note 1) — commas are not valid there.

`bitset_type`'s first argument must resolve (in sema) to a named enum type — a plain `enum {}` with no parenthesized backing type is fine and defaults to `i32`, same as an ordinary enum declaration. The second argument, if present, must resolve to one of `u8`/`u16`/`u32`/`u64`; if omitted it defaults to `u32`. Unlike `named_type` alone, a `bitset(...)` type always declares a new, distinct type — never an alias. See spec.md's "Bitset Types" section for the full semantics.

---

## Statements

```ebnf
stmt          ::= block_stmt
               | if_stmt
               | while_stmt
               | for_stmt
               | switch_stmt
               | var_decl_stmt
               | continue_stmt
               | break_stmt
               | return_stmt
               | return_err_stmt
               | return_ok_stmt
               | defer_stmt
               | when_stmt
               | link_decl          (* legal anywhere a stmt is, but always a SEMA error here —
                                        see spec.md; parses so the diagnostic can name it precisely *)
               | diagnostic_decl    (* same as link_decl above — always a SEMA error as a stmt *)
               | asm_stmt           (* legal only inside a function body — see below *)
               | expr_stmt

block_stmt    ::= '{' { stmt } '}'

if_stmt       ::= 'if' expr stmt [ 'else' stmt ]

while_stmt    ::= 'while' expr block_stmt

when_stmt     ::= 'when' expr block_stmt [ 'else' ( when_stmt | block_stmt ) ]

for_stmt      ::= 'for' for_binding 'in' for_iterable block_stmt

for_binding   ::= '&' IDENT                          (* for &val in ...: element bound by reference *)
               | IDENT [ ',' [ '&' ] IDENT ]         (* for val in ...  OR  for idx, [&]val in ... *)

for_iterable  ::= '..' expr                          (* upper-bound-only range; lower defaults to 0 *)
               | expr [ '..' expr ]                  (* a slice/array, or a lower..upper range *)

switch_stmt   ::= 'switch' expr '{' [ switch_arm { ',' switch_arm } [ ',' ] ] '}'

switch_arm    ::= arm_pattern ':' stmt

continue_stmt ::= 'continue'

break_stmt    ::= 'break'

return_stmt   ::= 'return' [ expr { ',' expr } ]

return_err_stmt ::= 'return_err' expr

return_ok_stmt  ::= 'return_ok' [ expr { ',' expr } ]

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

`when_stmt`'s `expr` must be a compile-time constant expression (a sema
error otherwise — see spec.md). Both branches are always type-checked; only
the selected branch's statements are emitted by codegen. The `then` branch
(and each block in an `else`/`else when` chain) must be a literal
`block_stmt`, unlike `if_stmt`'s `stmt` which accepts any statement.

### Asm Statement

```ebnf
asm_stmt      ::= 'asm' '{' <raw text, brace-balanced> '}'
```

The lexer captures everything between the outer `{` `}` as one raw token —
the body is never tokenized with the surrounding Mirage grammar above. It is
instead lexed and parsed by a second, wholly independent grammar, given
below, whose token vocabulary shares nothing with the rest of this document
(no `IDENT`/`INT_LITERAL`/etc. from the main grammar apply inside an asm
block):

```ebnf
asm_instr     ::= ASM_MNEMONIC [ asm_operand { ',' asm_operand } ]

asm_operand   ::= ASM_REGISTER
               | ASM_IMMEDIATE
               | ASM_IDENTIFIER                     (* a Mirage variable, read by value *)
               | '&' ASM_IDENTIFIER                 (* a Mirage variable, by address *)

ASM_MNEMONIC  ::= LETTER { LETTER | DIGIT | '_' }    (* not validated by the parser — an
                                                         instruction line's first word;
                                                         unrecognized mnemonics are a sema
                                                         warning, not a parse error *)

ASM_REGISTER  ::= (* one of the 64 general-purpose x86-64 register names, case-insensitive —
                     see spec.md's "Inline Assembly" section for the full table *)

ASM_IMMEDIATE ::= [ '-' ] DIGIT { DIGIT }
               | [ '-' ] '0x' HEX_DIGIT { HEX_DIGIT }

ASM_IDENTIFIER ::= LETTER { LETTER | DIGIT | '_' }   (* any word that isn't a register name *)
```

One instruction per line; a newline (or a run of them, collapsing to one)
separates instructions. `;` and `#` start a comment running to end of line,
discarded. Zero, one, two, or three operands are all legal per instruction
(arity legality per mnemonic is a sema concern, not a parse error). See
spec.md's "Inline Assembly" section for the full semantics: variable
resolution, the Tier-1/Tier-2 operand-direction analysis, implicit clobbers,
clobber-set construction, and width-mismatch checking.

`asm` is legal only inside a function body — a sema error otherwise (see
the top-level `declaration` grammar above). SSE/AVX/FPU registers, segment
registers, control/debug registers, and `[reg+disp]`-style memory operands
are recognized (so they can be named precisely in a diagnostic) but are not
supported in v1.

---

## Expressions

Expressions are parsed via precedence climbing. Listed from lowest to highest precedence:

```ebnf
expr          ::= assign_expr
               | import_expr          (* only in const := position *)

import_expr   ::= 'import' '(' STRING ')' { '.' IDENT }

assign_expr   ::= when_expr [ assign_op assign_expr ]

assign_op     ::= '=' | '+=' | '-=' | '*=' | '/='
               | '&=' | '|=' | '^=' | '<<=' | '>>=' | '~='

when_expr     ::= ternary_expr [ 'when' ternary_expr 'else' expr ]

ternary_expr  ::= logical_or_expr [ '?' expr ':' expr ]

logical_or_expr  ::= logical_and_expr { '||' logical_and_expr }

logical_and_expr ::= in_expr          { '&&' in_expr }

in_expr          ::= bitwise_or_expr  [ 'in' bitwise_or_expr ]

bitwise_or_expr  ::= bitwise_xor_expr { '|'  bitwise_xor_expr }

bitwise_xor_expr ::= bitwise_and_expr { ( '^' | '~' ) bitwise_and_expr }

bitwise_and_expr ::= equality_expr    { '&'  equality_expr }

equality_expr    ::= comparison_expr  { ( '==' | '!=' ) comparison_expr }

comparison_expr  ::= shift_expr  { ( '<' | '>' | '<=' | '>=' ) shift_expr }

shift_expr    ::= additive_expr { ( '<<' | '>>' ) additive_expr }

additive_expr ::= mult_expr  { ( '+' | '-' ) mult_expr }

mult_expr     ::= unary_expr { ( '*' | '/' | '%' ) unary_expr }

unary_expr    ::= try_expr
               | ( '-' | '!' | '~' | '&' | '++' | '--' ) unary_expr
               | postfix_expr

(* NOTE: dereference is NOT a prefix operator in Mirage — there is no C-style '*p'.
   It is the postfix_op '.' '*' below: 'p.*' *)

try_expr      ::= 'try' postfix_expr

postfix_expr  ::= primary_expr { postfix_op }

postfix_op    ::= '(' [ arg { ',' arg } ] ')'    (* call *)
               | '.' IDENT                        (* member access — auto-derefs through a pointer *)
               | '.' IDENT '{.' field_init { ',' field_init } '}'  (* qualified tagged variant constructor *)
               | '.' '*'                           (* dereference: 'p.*' reads/writes the pointee *)
               | '[' expr ']'                     (* index *)
               | '[' expr '..' expr ']'           (* slice *)
               | '++'
               | '--'

arg           ::= expr '...'                      (* spread — expr must be a slice; only legal as the
                                                       sole, final argument of a native-variadic call *)
               | expr
```

`~` is both a prefix `unary_expr` operator (bitwise/bitset complement, `~a`) and — as of `bitwise_xor_expr` above — an infix operator at the same precedence and with the same lowering as `^` (bitwise XOR for integers; symmetric difference for two operands of the same bitset type). There is no ambiguity: precedence-climbing means `~` is read as prefix only when no left operand has been parsed yet (the start of a `unary_expr`), and as infix once `bitwise_xor_expr`'s loop is looking for an operator after a complete left operand. `a ~ ~b` therefore parses as `a ^ (~b)`. Outside bitset operands, infix `~` behaves exactly like `^` (no separate meaning).

`in_expr` (`expr in expr`) is bitset membership testing — see spec.md's "Bitset Types" section for the legal LHS/RHS shapes and semantics. It binds looser than every arithmetic/bitwise/comparison operator, tighter than `&&`/`||`, and does not chain (`a in b in c` is not meaningful, since the result of `in` is `bool`, not a bitset). This is a **different** `in` from the one in `for_binding ... 'in' for_iterable` (see Statements below): the `for` statement's `in` is consumed directly as part of its own restricted grammar and never reaches general expression parsing, so the two never conflict — a `for` loop's iterable expression may itself legally contain an `in_expr`, e.g. `for x in (a in b)`.

`when_expr` (`then_val when cond else else_val`) binds looser than every
binary operator including ternary `?:`, but tighter than assignment — it sits
between `assign_expr` and `ternary_expr`. `then_val` and `cond` each parse at
the `ternary_expr` tier; `else_val` is a full `expr` (right-recursive, so
`when` chains and composes with assignment the same way ternary's own `else`
branch does). Composition examples:

```mirage
a + b when c else d        # parses as: (a + b) when (c) else (d)
x when a && b else y       # parses as: x when (a && b) else y
z = a when b else c        # parses as: z = (a when b else c)
a when b else c when d else e   # parses as: a when b else (c when d else e)
```

A bare, unparenthesized nested `when...else` in `cond` position does not
parse (it needs parens: `x when (a when b else c) else y`) — this is
deliberate, not an oversight, and avoids any ambiguity with `?:`.

When `cond` is a compile-time constant expression, only the selected
branch is emitted by codegen (both branches are still type-checked). When
`cond` is a runtime value, `when_expr` behaves exactly like `ternary_expr`
(both branches type-checked AND emitted). See spec.md's "Compile-Time
Configuration" section.

### Primary Expressions

```ebnf
primary_expr  ::= INT_LITERAL
               | FLOAT_LITERAL
               | STRING_LITERAL
               | CHAR_LITERAL
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
               | stackalloc_expr
               | import_bin_expr
               | match_expr
               | braced_initializer
               | dot_ident_expr
               | contextual_tagged_variant
               | option_expr
               | env_expr

option_expr   ::= '@option' '(' STRING [ ',' expr ] ')'

env_expr      ::= '@env' '(' STRING [ ',' expr ] ')'

sizeof_expr   ::= 'sizeof' '(' sizeof_operand ')'

sizeof_operand ::= type    (* whenever the token(s) can only start a type: a builtin type
                               keyword, '*', '[', 'struct', 'enum', 'union', 'bitset', 'fn', 'trait' *)
               | expr      (* otherwise — may still simply name a type, e.g. a module member *)

len_expr      ::= 'len' '(' expr ')'

cast_expr     ::= 'cast' '(' expr ',' type [ ',' expr ] ')'

stackalloc_expr ::= 'stackalloc' '(' expr ')'

import_bin_expr ::= 'import_bin' '(' STRING ')'

match_expr    ::= 'match' expr '{' [ match_arm { ',' match_arm } [ ',' ] ] '}'

match_arm     ::= arm_pattern ':' expr

arm_pattern   ::= '.' IDENT [ '(' [ '&' ] IDENT ')' ]    (* variant pattern *)
               | '_'                                        (* default/wildcard *)
               | expr                                       (* literal pattern, must be constant *)

dot_ident_expr ::= '.' IDENT   (* enum literal; valid where enum type is expected *)
               | '.' IDENT '(' expr ')'   (* sugar for '.' IDENT '{.v = expr}' — see below *)

contextual_tagged_variant ::= '.' IDENT '{.' field_init { ',' field_init } '}'

field_init    ::= '.' IDENT '=' expr

bitset_literal ::= '{' '.' IDENT { ',' '.' IDENT } '}'           (* bitset member set; only legal
                                                                      where a bitset type is expected *)

braced_initializer ::= '{' '}'                                        (* empty *)
               | '{' field_init { ',' field_init } '}'             (* struct fields *)
               | bitset_literal
               | '{' expr { ',' expr } [ '...' ] '}'                  (* array values, optional trailing fill *)
```

`braced_initializer`'s struct-fields and bitset-member-set alternatives are
disambiguated by whether `=` follows the first `.IDENT`: `{.field = expr,
...}` is a struct literal, `{.A, .B}` (no `=`) is a bitset literal. Mixing
the two forms in the same braced initializer (e.g. `{.A, .B = c}`) is a
parse error. `{}` (empty) is shared: with a bitset-expected type it denotes
the zero value (no bits set), exactly as `default` does for a bitset type
(see spec.md).

`@` is a sigil, not part of an identifier; `option`/`env` (like `link` in
`link_decl` above) are parsed as plain identifiers immediately following it,
not reserved keywords. `option_expr`/`env_expr` are legal anywhere
`primary_expr` is legal — nested inside arithmetic, as a call argument
(including `@link`'s `data`), as a `mut` initializer, etc. Their value is
resolved once — from `--opt`/the default for `option_expr`, from the named
environment variable/the default for `env_expr` — and cached, so each
composes as an ordinary compile-time-constant expression wherever it's
written. See spec.md's "Compile-Time Configuration" section for the
target-type resolution priority and value-coercion rules (identical for
both).

---

## Literals

```ebnf
INT_LITERAL   ::= decimal_int | hex_int | bin_int

decimal_int   ::= DIGIT { DIGIT | '_' }
hex_int       ::= '0x' HEX_DIGIT { HEX_DIGIT | '_' }
bin_int       ::= '0b' BIN_DIGIT { BIN_DIGIT | '_' }

FLOAT_LITERAL ::= DIGIT { DIGIT } '.' DIGIT { DIGIT }

STRING_LITERAL ::= '"' { char | escape_seq } '"'

CHAR_LITERAL   ::= "'" ( char | escape_seq ) "'"          (* type u8, not a distinct char type *)

escape_seq    ::= '\\' | '\"' | "\'" | '\n' | '\t' | '\r'
               | '\x' HEX_DIGIT HEX_DIGIT                (* exactly two hex digits *)
               | '\' OCTAL_DIGIT [ OCTAL_DIGIT [ OCTAL_DIGIT ] ]   (* 1-3 octal digits, max 0xFF *)

OCTAL_DIGIT   ::= '0' .. '7'

IDENT         ::= LETTER { LETTER | DIGIT | '_' }

DIGIT         ::= '0'..'9'
HEX_DIGIT     ::= '0'..'9' | 'a'..'f' | 'A'..'F'
BIN_DIGIT     ::= '0' | '1'
LETTER        ::= 'a'..'z' | 'A'..'Z' | '_'
```

---

## Notes on Syntax Conventions

1. **Struct field separators**: Struct fields in type definitions need no separator token at all between them — each field is self-delimiting, so writing one per line reads naturally. A `;` may optionally be sprinkled anywhere between (or after) fields with no semantic effect (the parser just skips any number of them) — this is how a single-line form like `struct { w: f64; h: f64 }` works. Commas are **not** valid here. The same "no separator required, `;` optionally tolerated" rule applies to enum variants, union members, and block statements. In `StructExpr` (braced init), fields use `.name = val` syntax with commas instead: `{.x = 1, .y = 2}`.

2. **Match/switch arm separator**: Arms are comma-separated; a trailing comma before `}` is optional.

3. **Multi-return**: Return values are comma-separated: `return a, b`. Multi-return type annotations use parentheses: `-> (T1, T2)`.

4. **`try` precedence**: `try` binds tighter than binary operators: `try f(x) + g()` parses as `(try f(x)) + g()`. To chain member access after try, use parentheses: `(try f(x)).field`.

5. **Braced initializer disambiguation**: A `{` followed by `.identifier` signals a struct or tagged union field initializer. `{` followed by anything else is an array initializer, empty initializer `{}`, or a block statement.

6. **`ext` keyword**: `ext` is scanned as an `IDENT` with the lexeme `"ext"`, not as a keyword token. It is valid only at the start of a declaration in `ext fn name(...)` form.

7. **`_` in group declarations**: The underscore `_` is recognized as an `IDENT` with value `"_"` in group declarations and match default patterns; it is not a keyword.

8. **`...` is lexically one token used in five unrelated grammar positions**, disambiguated purely by parse context — never by a shared representation:
   - *Native variadic parameter* (`param`): `name: ...T` — a type follows the dots; the parameter dissolves to `[]T` inside the function body. Only legal as the final parameter of a `fn`.
   - *`ext fn` C-varargs* (`ext_params`): a bare trailing `...` with no type, requiring at least one named parameter before it.
   - *`fn(...)` function-pointer-type C-varargs* (`fn_type_params`): a bare trailing `...` with no type, same C-vararg semantics as `ext fn`.
   - *Array-fill initializer* (`braced_initializer`): trailing `...` after the last element of `{ expr, ... }` repeats it (see note 10).
   - *Call-argument spread* (`arg`): `expr...` forwards an existing slice as a variadic argument; only legal as the sole, final argument of a call whose callee has a native `...T` parameter.

9. **`for` statement**: Implemented as `for_stmt` above. `for val in iterable`, `for idx, val in iterable`, and `for &val`/`for idx, &val` (element bound by reference) are all supported. `iterable` is a slice, a fixed-size array, or a range (`lower..upper`, or `..upper` with an implicit lower bound of 0).

10. **Array fill `...` in array initializer**: In an array initializer `{ expr, ... }`, a trailing `...` immediately after the last value repeats that value (evaluated once) to fill all remaining elements of the array. It must be the last token before `}`.

11. **Inferred array size `[?]T`**: Valid only as the declared type of a `const`/`let` (or local `const`/`mut`) declaration. The element count is taken directly from the initializer, which must be a literal array initializer `{ ... }` with no trailing `...` fill (note 10) and must not be an identifier, function call, or other computed expression.

12. **`sizeof`'s operand may be a type or an expression**: the parser looks ahead to decide — a built-in type keyword, or a token that can *only* begin a type (`*`, `[`, `struct`, `enum`, `union`, `fn`, `trait`), is parsed as `type`; anything else is parsed as an ordinary `expr` (which may itself simply name a type, e.g. `sizeof(module.TypeName)`). `sizeof(u64)`, `sizeof(*u8)`, and `sizeof([]T)` are all valid.

13. **`stackalloc` vs. `import_bin`**: both are primary expressions shaped like a builtin call (`'ident' '(' ... ')'`), same as `sizeof`/`len`/`cast`, but neither takes a type argument: `stackalloc` takes a single size `expr` and evaluates to `anyptr`; `import_bin` takes a single `STRING` path and evaluates to a compile-time `[N]u8` constant.
