# Mirage Grammar (EBNF)

This grammar is derived directly from the parser in `src/compiler/ast.cpp`. Terminals are written in `'single quotes'` or as `UPPERCASE` for token classes. Optional parts are in `[brackets]`. Repetition is `{...}`. Alternatives are `|`.

---

## Top-Level Structure

```ebnf
program       ::= { declaration } EOF

declaration   ::= [ attribute ] [ 'pub' ] fn_decl   (* attribute: also legal on method_decl inside impl blocks — see below *)
               | [ 'pub' ] ext_fn_decl
               | [ 'pub' ] type_decl
               | [ 'pub' ] var_decl
               | [ 'pub' ] macro_decl
               | impl_decl           (* impl cannot be pub *)
               | link_decl           (* cannot be pub *)
               | diagnostic_decl     (* cannot be pub *)
               | when_decl           (* cannot be pub *)
               | bare_import_decl    (* cannot be pub; module scope only — unlike
                                         link_decl/diagnostic_decl/asm_stmt above, a bare
                                         'import(...)' in statement position is a PARSE
                                         error, not a sema error, so it is NOT part of
                                         'stmt' below *)
               | asm_stmt            (* cannot be pub; always a SEMA error at module scope —
                                         parses here (like link_decl/diagnostic_decl above)
                                         purely so the diagnostic can name it precisely *)

bare_import_decl ::= 'import' '(' STRING ')'
```

---

## Declarations

### Function Declaration

```ebnf
fn_decl       ::= [ attribute ] 'fn' IDENT [ generic_params ]
                  '(' [ param { ',' param } ] ')' [ return_types ] stmt

param         ::= [ 'mut' ] IDENT ':' type [ '=' expr ]  (* typed, optional default *)
               | [ 'mut' ] IDENT ':=' expr               (* inferred type, default required *)
               | IDENT ':' '...' type                    (* native variadic; must be the last parameter *)

return_types  ::= '->' ret_item                                  (* single return *)
               | '->' '(' ret_item { ',' ret_item } ')'         (* multi-return *)

ret_item      ::= [ IDENT ':' ] [ '?' ] type   (* the 'IDENT :' name is optional and purely
                                                  cosmetic — self-documenting, no functional
                                                  effect (no implicit bare-return binding).
                                                  '?' marks an ignorable error and is legal
                                                  ONLY on the last ret_item — see note 19 *)
```

`pub`, if present, comes after the attribute: `@naked pub fn f() { ... }`, never `pub @naked fn f() { ... }`.

A return type (single or, per-entry, within a multi-return list) may optionally be named —
`-> (index: usize, found: bool)` — purely to make the signature self-documenting (e.g. in
LSP hover). The name has no functional effect: it does not create an implicit binding, and
`return` still requires explicit values (`return 0, false`), exactly as with unnamed return
types. Naming is independent per entry — a multi-return list may mix named and unnamed
entries.

### Declaration Attributes

```ebnf
attribute     ::= '@' IDENT                                 (* single, no arguments *)
               | '@' IDENT '(' expr { ',' expr } ')'       (* single, with arguments *)
               | '@' '(' IDENT { ',' IDENT } ')'           (* grouped — one or more, no arguments *)
```

One attribute clause may precede a `fn_decl` or a `method_decl` (a method inside an `impl`
block) — these are the only declaration kinds attributes are legal on. Multiple separate
clauses on the same declaration (`@naked @no_return`) are a parse error; use the grouped form
instead (`@(naked, no_return)`). A grouped-form member never takes its own argument list —
`@(section(".text"))` is a parse error; only the ungrouped `@name(args)` form takes arguments.
`IDENT` here (`no_return`, `naked`, `always_inline`, `section`, `init`) is validated against
the fixed known-attribute set by the parser, the same way `link_decl`'s category name is —
see spec.md's "Declaration Attributes" section for each attribute's semantics. `init` is
additionally rejected specifically on a `method_decl` by sema (a structural restriction, not
a parser-level one) — see spec.md's `@init` section.

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
type_decl     ::= 'type' IDENT [ generic_params ] '=' type
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
impl_decl     ::= 'impl' named_type [ generic_params ] '{' { method_decl } '}'                    (* bare impl *)
               | 'impl' named_type 'for' named_type [ generic_params ] '{' { method_decl } '}'  (* trait impl *)

generic_params ::= '[' generic_param { ',' generic_param } ']'

generic_param  ::= IDENT ':' type   (* the type after ':' must resolve, in sema, to one of:
                                        the builtin 'type' keyword (an unconstrained type
                                        parameter); the name of a TRAIT (a bounded type
                                        parameter, e.g. 'T: Hashable'); or one of the builtin
                                        scalar types 'bool'/an integer kind/'usize' (a value,
                                        "const-generic" parameter, e.g. 'N: usize').
                                        Anything else — a struct, an enum, another generic
                                        type, f32/f64, ... — is a sema error. Note a NAMED type
                                        here is accepted on shape by the parser and checked for
                                        trait-ness later, after every trait is registered. See
                                        spec.md §22 "Generics" for the full semantics. *)

method_decl   ::= [ attribute ] [ 'pub' ] 'fn' IDENT
                  '(' ( 'self' | 'mut' 'self' )
                      { ',' ( [ 'mut' ] IDENT ':' type [ '=' expr ]
                            | [ 'mut' ] IDENT ':=' expr ) }
                  ')'
                  [ return_types ]
                  stmt
```

`method_decl` never carries its own `generic_params` — only the enclosing `impl`
block does (a method's parameters are tied to whichever concrete instantiation
the `impl` block itself is applied to; see spec.md §22 "Generics").

An attribute clause on a `method_decl` follows the same ordering as `fn_decl` (attribute
before `pub`) and accepts the same five known names, though sema rejects `init` specifically
on a method — see spec.md's "Declaration Attributes" section for each attribute's semantics.

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
link_decl     ::= '#link' '(' link_category ',' expr ')'

link_category ::= 'lib' | 'system' | 'flag'
```

`#` is a sigil, not part of an identifier; `link` (like `ext` above) is parsed
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

diagnostic_kw   ::= 'error' | 'warn'    (* '#error'/'#warn'; 'error' is the KwError
                                            keyword, 'warn' a plain identifier *)
```

`#` is a sigil, not part of the keyword/identifier that follows it. A
compile-time diagnostic directive: `#error` emits a sema error, `#warn` a
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
restricts the permitted kinds to `#link`, `#error`, `#warn`, `const` with
`$option`/`$env`, `type`, and `ext fn` (see spec.md's "Compile-Time
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
               | trait_type                             (* trait handle; see Traits below *)
               | error_type
               | bitset_type
               | fn_type
               | named_type
               | builtin_type

error_type    ::= 'error' '(' named_type { '|' named_type } ')'

bitset_type   ::= 'bitset' '(' named_type [ ',' builtin_type ] ')'

fn_type       ::= 'fn' '(' [ fn_type_params ] ')' [ '->' fn_ret_item
                                                  | '->' '(' fn_ret_item { ',' fn_ret_item } ')' ]

fn_ret_item   ::= [ '?' ] type   (* '?' legal only on the last item — see note 19 *)

fn_type_params ::= fn_type_param { ',' fn_type_param } [ ',' '...' ]
                | '...'

fn_type_param ::= [ IDENT ':' ] type

named_type    ::= IDENT { '.' IDENT } [ generic_args ]

generic_args  ::= '[' generic_arg { ',' generic_arg } ']'

generic_arg   ::= type   (* for a 'T: type' parameter *)
               | expr    (* for a value parameter — must be a compile-time constant expression
                             of the corresponding parameter's declared scalar type; same
                             type-vs-expr lookahead disambiguation as size_of_operand (note 12)
                             applies here token-for-token *)

builtin_type  ::= 'u8' | 'u16' | 'u32' | 'u64'
               | 'i8' | 'i16' | 'i32' | 'i64'
               | 'f32' | 'f64'
               | 'usize' | 'bool' | 'byte' | 'anyptr'
               | 'type' | 'any'

trait_type    ::= 'trait' '{' { trait_method_decl } '}'
               | 'trait' '(' named_type { ',' named_type } ')' [ '{' { trait_method_decl } '}' ]

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

**Trait composition**: `trait_type`'s second alternative, `trait(A, B, ...)
{ ... }`, declares that this trait *composes* one or more other traits — each
entry is a `named_type` (`allow_generic_args` suppressed, same as
`impl_decl`'s trait/type operands above, since traits are never generic). The
brace body is optional when a composition list is present, but if written
must be non-empty (the same empty-body rule as the bare form above). A
composing trait's effective method set is the union of its own declared
methods (if any) plus every method reachable, transitively, through its
composed traits — see spec.md's "Trait Composition" section for flattening,
collision, and cycle-detection semantics, none of which are syntactic (a
composed-list entry that doesn't name a trait, a duplicate entry, or a cycle
are all sema errors, not parse errors).

A trait method may declare default parameter values, following the same
rules as `fn_decl`'s `param` above. When a trait handle (`dyn`-style dynamic
dispatch) call omits an argument, the default always comes from the trait's
own declaration — never from whichever concrete impl happens to be behind
the handle — and is resolved before the vtable call.

Note: Struct, enum, and union fields/variants need no separator token between them at all (see Notes on Syntax Conventions, note 1) — commas are not valid there.

`bitset_type`'s first argument must resolve (in sema) to a named enum type — a plain `enum {}` with no parenthesized backing type is fine and defaults to `i32`, same as an ordinary enum declaration. The second argument, if present, must resolve to one of `u8`/`u16`/`u32`/`u64`; if omitted it defaults to `u32`. Unlike `named_type` alone, a `bitset(...)` type always declares a new, distinct type — never an alias. See spec.md's "Bitset Types" section for the full semantics.

A `named_type` naming a declaration with `generic_params` is never complete without `generic_args` — `List` alone is not a valid type; `List[i32]` (or `Fixed[u8, 16]` for a mixed parameter list) is required at every use, in declared parameter order, except inside the declaration's own body/signature where the implicit self-instantiation rule applies (see spec.md §22 "Generics", and notes 15-18 below for the parse-time disambiguations this construct requires).

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

### Asm Expression

```ebnf
asm_expr      ::= 'asm' '->' ASM_REGISTER [ ':' type ] '{' <raw text, brace-balanced> '}'
```

Unlike `asm_stmt` above, `asm_expr` is a `primary_expr` — legal anywhere an
expression is legal (`return`, a `var_decl_stmt` initializer, a call
argument, ...), not just as a bare statement. `ASM_REGISTER` here is parsed
by the MAIN parser (not the standalone asm-body grammar) using the same
register table as `asm_stmt`'s body; an unrecognized identifier is a parse
error naming the construct. The raw body between `{` `}` is captured and
parsed by the exact same standalone asm-body grammar given above for
`asm_stmt` — only the header (`'->' ASM_REGISTER [':' type]`) differs.

`ASM_REGISTER` names the register whose value at the end of the block
becomes the expression's value. The optional `': type'` gives the result
type explicitly; when absent, the result type is inferred from the
surrounding expected-type context (the declared type of a `var_decl_stmt`
being initialized, the enclosing function's declared return type, etc.) —
a sema error if neither is available. See spec.md's "Inline Assembly"
section for the exact diagnostics (cannot-infer error, result-register
width-mismatch warning) and the rule that the result register is always an
implicit clobber, even when no instruction in the block explicitly writes
it. A `'&' ASM_IDENTIFIER` memory-write operand may appear in the same
block as the register-return mechanism — the two outputs are independent
and compose freely.

---

## Expressions

Expressions are parsed via precedence climbing. Listed from lowest to highest precedence:

```ebnf
expr          ::= assign_expr
               | import_expr          (* only in const := position *)

import_expr   ::= 'import' '(' STRING ')' { '.' IDENT }
```

There are two distinct, unrelated-looking-but-textually-identical uses of `'import' '(' STRING ')'` in this grammar: `import_expr` above (only reachable as a `const` initializer — `const mod := import("path")` — binding the target module as a namespace) and `bare_import_decl` (a standalone module-scope *declaration* — `import("path")` with no `const`/binding at all), which instead injects every `pub` symbol of the target module into the current module as private, unqualified local names, with no namespace binding created. The parser distinguishes them purely by position: `import(...)` right after `const NAME :=` parses as `import_expr`; `import(...)` at the start of a declaration parses as `bare_import_decl`. See spec.md's "Bare Import" section (under Modules) for the full semantics.

```ebnf
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
               | '[' expr ']'                     (* index — see note 16: a single-item, non-slice
                                                       '[...]' here is syntactically ambiguous with
                                                       explicit generic-argument instantiation and is
                                                       classified in sema, not by the parser *)
               | '[' [ expr ] '..' [ expr ] ']'   (* slice — never ambiguous, see note 16.
                                                       Both bounds are optional: an absent lower
                                                       bound is 0, an absent upper bound is the
                                                       operand's length, so '[..]' is the whole
                                                       operand *)
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
a + b when c else d        // parses as: (a + b) when (c) else (d)
x when a && b else y       // parses as: x when (a && b) else y
z = a when b else c        // parses as: z = (a when b else c)
a when b else c when d else e   // parses as: a when b else (c when d else e)
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
               | size_of_expr
               | align_of_expr
               | type_of_expr
               | type_info_of_expr
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
               | asm_expr

option_expr   ::= '$option' '(' STRING [ ',' expr ] ')'

env_expr      ::= '$env' '(' STRING [ ',' expr ] ')'

size_of_expr  ::= 'size_of' '(' size_of_operand ')'

size_of_operand ::= type    (* whenever the token(s) can only start a type: a builtin type
                               keyword, '*', '[', 'struct', 'enum', 'union', 'bitset', 'fn', 'trait' *)
               | expr      (* otherwise — may still simply name a type, e.g. a module member *)

align_of_expr ::= 'align_of' '(' size_of_operand ')'  (* same type-vs-expr disambiguation as size_of *)

type_of_expr  ::= 'type_of' '(' size_of_operand ')'   (* same type-vs-expr disambiguation as size_of *)

type_info_of_expr ::= 'type_info_of' '(' expr ')'    (* operand is always a plain expr, e.g.
                                                          'type_info_of(type_of(i32))' *)

len_expr      ::= 'len' '(' expr ')'

cast_expr     ::= 'cast' '(' expr ',' type [ ',' expr ] ')'

stackalloc_expr ::= 'stackalloc' '(' expr ')'

asm_expr      ::= 'asm' '->' ASM_REGISTER [ ':' type ] '{' <raw text, brace-balanced> '}'

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

`$` is a sigil, not part of an identifier — distinct from the `#` sigil used
by `link_decl`/`diagnostic_decl` above. `option`/`env` are parsed as plain
identifiers immediately following it, not reserved keywords.
`option_expr`/`env_expr` are legal anywhere `primary_expr` is legal —
nested inside arithmetic, as a call argument (including `#link`'s `data`),
as a `mut` initializer, etc. Their value is resolved once — from
`--opt`/the default for `option_expr`, from the named environment
variable/the default for `env_expr` — and cached, so each composes as an
ordinary compile-time-constant expression wherever it's written. See
spec.md's "Compile-Time Configuration" section for the target-type
resolution priority and value-coercion rules (identical for both).

---

## Literals

```ebnf
INT_LITERAL   ::= decimal_int | hex_int | bin_int | octal_int

decimal_int   ::= DIGIT { DIGIT | '_' }
hex_int       ::= '0x' HEX_DIGIT { HEX_DIGIT | '_' }
bin_int       ::= '0b' BIN_DIGIT { BIN_DIGIT | '_' }
octal_int     ::= '0o' OCTAL_DIGIT { OCTAL_DIGIT | '_' }

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

## Comments

```ebnf
COMMENT       ::= line_comment | block_comment

line_comment  ::= '//' { any_char_except_newline }

block_comment ::= '/*' { any_char } '*/'      (* not nested; a newline inside still
                                                  counts as a line break for ASI purposes *)
```

Comments are treated as whitespace: they may appear anywhere whitespace is
allowed and carry no semantic meaning. An unterminated `/* ... */` is a
lexer error. See spec.md's "Comments" section for the general syntax, and
"Inline Assembly" (§20) for the asm mini-language's separate `;`/`#`
comment convention used *inside* `asm { ... }` bodies — that convention is
handled entirely by the asm block's own raw-text lexer and is unrelated to
the top-level sigils used above: outside an `asm { ... }` body, neither `#`
(`link_decl`/`diagnostic_decl`) nor `$` (`option_expr`/`env_expr`) ever
introduces a comment — `#` always starts a link/diagnostic directive, `$`
always starts an `option`/`env` expression.

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

12. **`size_of`'s operand may be a type or an expression**: the parser looks ahead to decide — a built-in type keyword, or a token that can *only* begin a type (`*`, `[`, `struct`, `enum`, `union`, `fn`, `trait`), is parsed as `type`; anything else is parsed as an ordinary `expr` (which may itself simply name a type, e.g. `size_of(module.TypeName)`). `size_of(u64)`, `size_of(*u8)`, and `size_of([]T)` are all valid. `align_of` (returning the operand's required alignment, also as `usize`) uses the exact same operand grammar and disambiguation rule.

13. **`stackalloc` vs. `import_bin`**: both are primary expressions shaped like a builtin call (`'ident' '(' ... ')'`), same as `size_of`/`len`/`cast`, but neither takes a type argument: `stackalloc` takes a single size `expr` and evaluates to `anyptr`; `import_bin` takes a single `STRING` path and evaluates to a compile-time `[N]u8` constant.

14. **`type_of`'s operand disambiguation matches `size_of`/`align_of`**: `type_of(u64)`, `type_of(*u8)`, and `type_of(SomeStruct)` are all valid, using the exact same type-vs-expr lookahead rule as note 12. `type_of`'s result is always the builtin `type` — a compile-time-unique identifier for the operand's type. It is a compile-time constant for every operand except one whose resolved type is `any`, which lowers to a runtime read of the `any` value's type id instead. `type_info_of`'s operand, by contrast, is always parsed as a plain `expr` (never type-disambiguated) — it must resolve to `type` or `any`, and always evaluates to `anyptr` (nil if no `Type_Info` exists for the referenced type, e.g. a builtin scalar).

15. **`generic_args` never conflicts with array-type syntax**: `[N]T`/`[]T`/`[?]T` are only ever reached by `parse_type`'s own dispatch on a *bare* leading `[` token (a `type` production alternative in its own right); `generic_args` is only ever reached as an optional suffix immediately following an `IDENT` that has already been parsed as `named_type`'s leaf segment. These are different grammar positions, so a type-position `[` is never ambiguous between the two: `List[i32]` is `named_type` `List` followed by `generic_args`; `[4]List` is the array-type production with element type `List`.

16. **`IDENT '[' ... ']'` at an expression/call site is ambiguous between ordinary indexing and explicit generic-argument instantiation, and the parser does not resolve it.** Ordinary indexing (`postfix_op`'s `'[' expr ']'`) is always exactly one `expr` with no comma; `generic_args` is a comma-separated list. So a bracket containing a comma can only be `generic_args` — no ambiguity there. A single-item bracket, however, is genuinely ambiguous by shape alone (`arr[i]` vs. `List[i32]` look identical at this point in parsing), and this parser has no symbol-table lookup available during parsing to resolve it the way `size_of`/`type_of`'s operand disambiguation can (that lookahead only needs token shape, not declaration knowledge). The parser instead emits one shared node capturing both possible readings, and sema classifies it once `IDENT`'s declaration is known — see spec.md §22 "Generics" for the resolution rule (does the identifier name a declaration with `generic_params`?). The slice form is never part of this ambiguity: a generic argument list cannot contain `..`, so the production is settled the moment one is seen — including in leading position (`arr[..5]`), which is why the parser must peek for `..` before attempting to parse a generic argument.

17. **`impl_decl`'s own `generic_params` clause, not `named_type`'s `generic_args`, governs the bracket after an impl target.** `named_type`'s `generic_args` is not consulted when parsing `impl_decl`'s `named_type` operand(s) — the `[...]` immediately following `impl SOME_TYPE` (or, in the trait-impl form, following either `named_type`) is always parsed as `impl_decl`'s own trailing `generic_params` clause. Consequently `impl List[T: type] { ... }` (declaring the impl's own parameters, written once against the unspecialized declaration) is the only legal form — `impl List[i32] { ... }` (a per-instantiation specialization impl) is not legal in v1.

18. **Each `generic_arg`'s type-vs-expr parse reuses note 12's lookahead rule verbatim**: a builtin type keyword, or a token that can only begin a type, parses as `type`; anything else parses as `expr`. This is the same rule `size_of_operand` and `generic_arg` both use, applied independently to each comma-separated item inside `generic_args`.

19. **`?` on the last return type marks an ignorable error**, and is the only place the parser accepts a leading `?` in type position. It appears in `ret_item` (free functions, `impl` methods, trait methods) and `fn_ret_item` (function-pointer types); whether it is on the *last* item cannot be known while parsing item *i*, so the parser records each marker's location and reports the non-final ones once the list is complete. Everywhere else `parse_type` rejects `?` outright with a dedicated diagnostic rather than a generic "expected type". What may follow the `?` is deliberately unconstrained by the grammar — `?error(A | B)`, `?SomeEnum` (sugar for `?error(SomeEnum)`), and `?SomeAlias` are all shaped like an ordinary `type` — because only sema can tell whether the named type is an error type at all. See spec.md §16 "Ignorable Errors".
