# Mirage Language Specification

## Overview

Mirage is a compiled, statically-typed systems language that targets native code via LLVM IR. It is designed for low-level programming with a clean, expression-oriented syntax. Mirage compiles to native executables and can interoperate with C libraries via `ext fn` declarations.

### Comments

```mirage
// a line comment, runs to end of line

/* a block comment,
   may span multiple lines */

const x: i32 = 1 /* inline */ + 2   // trailing
```

Line comments start with `//` and run to the end of the line. Block comments
are delimited by `/*` and `*/`, may span multiple lines, and do not nest — the
first `*/` closes the comment. An unterminated block comment is a compiler
error. Comments carry no semantic meaning and are treated as whitespace.

Note: the `asm { ... }` mini-language uses its own, separate comment
convention (`;` and `#`) inside the assembly body — see §20, "Inline
Assembly".

---

## 1. Primitive Types

### Integer Types

| Type    | Width   | Signedness  |
|---------|---------|-------------|
| `u8`    | 8-bit   | unsigned    |
| `u16`   | 16-bit  | unsigned    |
| `u32`   | 32-bit  | unsigned    |
| `u64`   | 64-bit  | unsigned    |
| `i8`    | 8-bit   | signed      |
| `i16`   | 16-bit  | signed      |
| `i32`   | 32-bit  | signed      |
| `i64`   | 64-bit  | signed      |
| `usize` | pointer-wide | unsigned |

### Floating-Point Types

| Type  | Width  |
|-------|--------|
| `f32` | 32-bit |
| `f64` | 64-bit |

### Other Scalar Types

| Type     | Description                                                    |
|----------|----------------------------------------------------------------|
| `bool`   | Boolean: `true` or `false`                                     |
| `byte`   | Alias for `u8`; used for raw byte buffers                      |
| `anyptr` | Untyped pointer; interoperates with all typed pointers and `nil` |

Note: `error` is not a scalar type — `error(T)` / `error(A \| B \| C)` is a
type-former used only in function return-type position. See §16, "Error
Type System".

### The `anyptr` Type

`anyptr` supports arithmetic with integer operands (`+`, `-`) for pointer arithmetic. It can be assigned to and from any typed pointer or function pointer, and compared with `nil`.

---

## 2. Composite Types

### Pointer Types

```
*T
```

A typed pointer to a value of type `T`. Address taken with unary `&`. Typed pointers are implicitly assignable to/from `anyptr`.

Dereferenced with the **postfix** `.*` operator — `p.*` reads or writes the pointee (there is no C-style prefix `*p`; a leading `*` in expression position is not a valid operator):

```mirage
mut x: i32 = 42
const p: *i32 = &x    // address-of
const v: i32 = p.*    // dereference: reads x through p
p.* = 99              // dereference: writes x through p
```

Auto-deref: accessing members or calling methods on a pointer-to-struct automatically dereferences the pointer, so `p.field` and `p.method()` never need an explicit `.*` — only reading/writing/matching the whole pointee value does.

### Array Types

```
[N]T
```

A fixed-size array of `N` elements of type `T`. `N` must be a compile-time constant expression. Arrays are value types (copied on assignment). Arrays are implicitly assignable to matching slice types.

```
[?]T
```

An array whose size is inferred from its initializer. Valid only as the declared type of a `const`/`let` declaration whose initializer is a literal array initializer with no `...` fill; the element count becomes the number of elements written. Once resolved, `[?]T` behaves identically to the equivalent `[N]T`.

### Slice Types

```
[]T
```

A fat pointer: a (data pointer, length) pair. Slices do not own memory. A slice into an array or pointer is created with `cast(ptr, []T, length)` or via `expr[start..end]`. Length is read with `len(slice)`. Slices are 16 bytes on 64-bit targets.

### Struct Types

```mirage
type Point = struct {
    x: i32 = 0
    y: i32 = 0
}
```

Structs are product types with named fields. Fields are separated by newlines. Each field may have an optional default initializer expression. Structs are laid out in declaration order with natural alignment.

**Packed structs** disable padding:
```mirage
type PackedHeader = struct(packed) {
    flags: u8
    length: u16
}
```

Fields are accessed with `.field`. Struct types and field names defined in one module can be used in other modules if the type is `pub`.

### Enum Types

```mirage
type Color = enum(u32) {
    red
    green = iota * 10
    blue
}
```

Enums are integer-backed named constants. The underlying type defaults to `i32` if omitted. Fields are separated by newlines. Field values are assigned starting from 0, or via an explicit expression. The special `iota` expression evaluates to the current field's sequential index and can be used in expressions.

**Iota**: when a field's initializer contains `iota`, that expression becomes the template for subsequent fields (substituting `iota` with each successive index).

Enum values are written as `.field_name` where the expected type provides context, or as `TypeName.field_name` when qualified.

### Union Types (Untagged)

```mirage
type NumUnion = union {
    as_i64: i64
    as_f64: f64
    as_u8:  u8
}
```

Untagged unions store all members at offset 0. The size equals the largest member's size rounded up to the maximum alignment. Member default initializers are not allowed.

Initialization requires exactly one named member:
```mirage
mut u: NumUnion = {.as_i64 = 42}
```

`undefined` is a valid initializer. `default` and `{}` are not valid.

### Tagged Union Types

```mirage
type Shape = union(enum) {
    circle: struct { radius: f64 }
    rect:   struct {
        w: f64
        h: f64
    }
    point
}
```

Tagged unions combine a `u32` discriminant tag with a payload. Each variant may optionally have a struct payload; payload-free variants are represented with no type annotation. The tag occupies bytes 0–3; the payload follows at an alignment-padded offset.

**Construction:**
```mirage
// Qualified form
const c: Shape = Shape.circle{.radius = 3.0}

// Contextual form (type inferred from annotation)
mut r: Shape = .rect{.w = 4.0, .h = 5.0}

// Payload-free
const p: Shape = .point
```

Payload fields in constructors use the `.field = value` syntax inside braces.

**Single-value payload sugar:** when a variant's payload struct has exactly
one field, `.variant(expr)` (unqualified only — no leading-type-name form)
is sugar for `.variant{.v = expr}`, where `v` is the field name every
non-struct payload (scalar, enum, union, slice, pointer, array) is wrapped
under:

```mirage
type IoError = union(enum) {
    NotFound: []u8
    Closed
}

const e: IoError = .NotFound("missing.txt")   // sugar for .NotFound{.v = "missing.txt"}
```

This never collides with an ordinary function call — a bare `.name` can
never resolve to a callable value, since `DotIdentExpr` always requires an
expected enum/tagged-union type.

Tagged union members cannot be accessed directly; use `match` to destructure.

**Implicit coercion:** wherever an expected type is known (call arguments, return statements,
variable initializers, struct/array/union field initializers), a value whose type exactly matches
the payload type of exactly one variant of the expected tagged-union type is automatically wrapped
in that variant — no explicit `TypeName.variant{...}` construction is required:

```mirage
type Arg = union(enum) {
    size: struct { value: usize }
    str:  struct { value: []u8 }
}

fn take(a: Arg) -> i32 { ... }

const n: usize = 42
take(n)          // implicitly wrapped as Arg.size{.value = n}
take("hello")    // implicitly wrapped as Arg.str{.value = "hello"}
```

The match is exact (the argument's type must equal the variant's single payload-field type
precisely, not merely be assignable to it) and requires the payload struct to have exactly one
field. If the argument's type doesn't exactly match any variant's single field, this coercion is
not attempted and normal type-checking rules apply. If it exactly matches more than one variant
(e.g. two variants both have a single `usize` field), the coercion is ambiguous and is a sema
error naming the union type and all matching variants — write an explicit
`TypeName.variant{...}` in that case.

### Function Pointer Types

```mirage
fn(ParamType1, ParamType2) -> ReturnType
fn(ParamType) -> (RetType1, RetType2)
fn(*u8, ...) -> i32
fn() -> void   // written as: fn()
```

Function pointer types represent callable values. They are opaque pointers internally (8 bytes). The `nil` literal is assignable to any function pointer type; `default` produces a null function pointer.

Multi-return function pointer types use `-> (T1, T2)` syntax.

---

## 3. Literals

### Integer Literals

```mirage
42          // decimal
0xFF        // hexadecimal
0b1010      // binary
0o400       // octal
1_000_000   // underscore separators allowed
```

### Float Literals

```mirage
3.14
2.0
```

### String Literals

```mirage
"hello\nworld"
```

Supported escape sequences: `\\`, `\"`, `\'`, `\n`, `\t`, `\r`, `\xNN` (exactly two hex digits), and
octal `\0` through `\777` (one to three octal digits, clamped to a single byte).

String literals have type `*u8` (null-terminated). The null terminator is appended automatically.

### Character Literals

```mirage
'a'
'\n'
'\x41'      // 'A'
```

A character literal is a single byte in single quotes, with the same escape sequences as string
literals. It has type `u8` — there is no distinct `char` type; a character literal is simply
convenient syntax for a `u8` value:

```mirage
fn is_digit(c: u8) -> bool {
    return c >= '0' && c <= '9'
}
```

### Boolean Literals

```mirage
true
false
```

### Nil Literal

```mirage
nil
```

The null pointer value. Assignable to any pointer, `anyptr`, or function pointer type.

### Enum Dot Literals

```mirage
.field_name
TypeName.field_name
```

An enum value written contextually (`.field_name`) is valid anywhere the expected type is an enum. The qualified form (`TypeName.field_name`) is valid anywhere.

---

## 4. Variables and Constants

### Mutable Variable Declaration

```mirage
mut name: Type = expr
mut name := expr          // type inferred from initializer
mut name: Type            // default-initialized (struct fields, arrays, etc.)
```

`mut` declares a mutable local variable or module-level global.

### Constant Declaration

```mirage
const name := expr
const name: Type = expr
```

`const` declares an immutable binding. A `const` requires an initializer. Module-level constants may use `import(...)` as the initializer to bind a module namespace.

### Group Declarations

```mirage
mut a, b := multi_return_call()
const x, y := divide(10, 3)
```

Group declarations destructure a multi-return function call. Names and return values are matched positionally. Use `_` to discard a value:

```mirage
mut val, _ := fallible_fn()
```

### The `default` Initializer

```mirage
mut x: i32 = default      // zero
mut p: Point = default    // each field default-initialized
```

`default` initializes a value to its type-appropriate zero. For structs, each field is recursively default-initialized (using field-level default expressions if present).

### The `undefined` Initializer

```mirage
mut x: i32 = undefined    // storage allocated, no initialization
```

`undefined` allocates storage but emits no initializer. Valid anywhere except `const` declarations. Use for performance-critical paths where initialization is immediately followed by an assignment.

### Module-Level Declarations

Module-level variables and constants use the same syntax. The `pub` modifier makes them visible to importing modules:

```mirage
pub mut global_counter: i32 = 0
pub const max_size: usize = 4096
```

---

## 5. Expressions

### Operator Precedence (low to high)

| Level | Operators                        | Associativity |
|-------|----------------------------------|---------------|
| 1     | `=` `+=` `-=` `*=` `/=` `&=` `\|=` `^=` `<<=` `>>=` `~=` | right |
| 2     | `when ... else ...`             | right         |
| 3     | `?:` (ternary)                  | right         |
| 4     | `\|\|`                           | left          |
| 5     | `&&`                             | left          |
| 6     | `in`                             | none (non-chaining) |
| 7     | `\|`                             | left          |
| 8     | `^` `~` (infix)                  | left          |
| 9     | `&`                              | left          |
| 10    | `==` `!=`                        | left          |
| 11    | `<` `>` `<=` `>=`               | left          |
| 12    | `<<` `>>`                       | left          |
| 13    | `+` `-`                          | left          |
| 14    | `*` `/` `%`                      | left          |
| 15    | Unary: `-` `!` `~` `&` `*` `++` `--` `try`| right         |
| 16    | Postfix: call `()` `.member` `[idx]` `++` `--` | left |
| 17    | Primary                          |               |

`~=` and infix `~` are legal on any bitset operand (see §18, "Bitset Types");
infix `~` behaves identically to `^` (bitwise XOR / symmetric difference).
`in` is bitset membership testing (§18) and is unrelated to `for ... in
...`'s `in` keyword (§6, "For Loop") — the two never conflict since `for`
consumes its `in` through a separate, restricted grammar.

### Arithmetic

```mirage
a + b    // add (also: anyptr + integer)
a - b    // subtract (also: anyptr - integer)
a * b
a / b
a % b
```

Both operands must have the same type (no implicit promotion). `anyptr` supports `+` and `-` with integer operands.

### Comparison

```mirage
a == b   // equal
a != b   // not equal
a < b
a > b
a <= b
a >= b
```

Result type is `bool`. Both operands must be of assignable types.

### Logical

```mirage
a && b   // logical AND (bool operands only)
a || b   // logical OR (bool operands only)
```

### Bitwise

```mirage
a & b    // bitwise AND
a | b    // bitwise OR
a ^ b    // bitwise XOR
a << b   // shift left
a >> b   // shift right
```

Both operands must have the same type.

**Bitset exception**: between two operands of the same bitset type, `+`, `-`, `&`, `^`, and `~` (infix) are also legal, with set-operation semantics (union, difference, intersection, symmetric difference) rather than plain-integer bitwise semantics — see §18, "Bitset Types". Mixing a bitset with a raw integer via any of these operators is a sema error; cast to the storage type first.

### Unary

```mirage
-x       // numeric negation
!x       // logical NOT (bool only)
~x       // bitwise NOT
&x       // address-of (produces *T)
```

Note: dereference is **not** in this list — it is the postfix `.*` operator (`x.*`), not a prefix operator. See [Pointer Types](#pointer-types).

### Ternary

```mirage
condition ? then_expr : else_expr
```

Both branches must have the same type.

### `when` Expression

```mirage
then_val when condition else else_val
```

Binds looser than every binary operator (including ternary `?:`) but tighter
than assignment. Both branches must have the same type (same rule as
ternary). Semantics depend on whether `condition` is a compile-time constant:

- **Compile-time constant**: both branches are still type-checked, but only
  the selected branch's code is emitted — the other branch's value is never
  computed at runtime.
- **Runtime value**: behaves exactly like the ternary `?:` above — both
  branches are type-checked and emitted, and the result is chosen at runtime.

See [Compile-Time Configuration](#compile-time-configuration) for how this
combines with `#option` and `#link`, and the `when` *statement* (a related
but distinct construct — see [Statements](#6-statements)).

### Assignment

```mirage
target = value
target += value
target -= value
target *= value
target /= value
target &= value
target |= value
target ^= value
target <<= value
target >>= value
```

The target must be a mutable lvalue (a `mut` variable, a dereference, an array index, or a struct member of a mutable value).

### Increment / Decrement

```mirage
x++    // post-increment
x--    // post-decrement
++x    // pre-increment  (prefix form in unary)
--x    // pre-decrement  (prefix form in unary)
```

Requires a mutable operand.

### Member Access

```mirage
value.field          // struct/union field
pointer.field        // auto-deref through pointer
module_name.symbol   // cross-module access
```

### Index and Slice

```mirage
arr[i]            // array or slice index
arr[start..end]   // slice expression (produces []T)
```

### Function Call

```mirage
fn_name(arg1, arg2)
obj.method(arg1)
fp(arg1)            // call through function pointer
mod.fn_name(arg)    // cross-module call
```

**Spread argument:** `expr...` forwards an existing slice as the variadic argument of a call to a
function with a native `...T` parameter (see [Variadic Arguments](#17-variadic-arguments)):

```mirage
fn sum(base: i32, nums: ...i32) -> i32 { ... }

fn forward(nums: []i32) -> i32 {
    return sum(0, nums...)
}
```

A spread argument must be the sole, final argument in the variadic slot — it cannot be combined
with additional loose variadic arguments (`f(a, xs..., b)` and `f(a, 1, xs...)` are both errors),
and it is only legal when the callee's variadic parameter is native `...T` (not an `ext fn`'s C
`...` varargs). The spread expression's type must be a slice assignable to `[]T`.

### `cast`

```mirage
cast(expr, TargetType)
cast(ptr, []T, length)   // create a slice from a pointer
```

Valid casts:
- Between any scalar types (integers, floats, bool, anyptr, pointers)
- `anyptr` ↔ any typed pointer
- `anyptr` ↔ function pointer type
- Array → slice (same element type)
- Pointer/anyptr → slice (requires length expression)

### `sizeof`

```mirage
sizeof(expr)
sizeof(TypeName)
sizeof(module.TypeName)
```

Returns the size in bytes as `usize`. The operand may be an arbitrary expression *or* a type written directly — including built-in type keywords: `sizeof(u64)`, `sizeof(*u8)`, `sizeof([]T)`, and `sizeof(fn(i32) -> i32)` are all valid. The parser disambiguates by looking ahead: anything that can only start a type (a built-in type keyword, `*`, `[`, `struct`, `enum`, `union`, `fn`, `trait`) is parsed as a type; otherwise the operand is parsed as a normal expression (which may itself simply name a type, e.g. `sizeof(module.TypeName)`).

### `len`

```mirage
len(array_or_slice)
```

Returns the number of elements as `usize`. Valid on array and slice types.

### `stackalloc`

```mirage
stackalloc(size)
```

Allocates `size` bytes on the current function's stack frame and returns an `anyptr` pointing at the start of that memory. The size expression need not be a compile-time constant — `stackalloc` supports runtime-sized allocations (a dynamic `alloca`), unlike a fixed-size array whose length must be known at compile time. Pair it with `cast(ptr, []T, len)` to view the raw memory as a typed slice:

```mirage
fn read_line(max_len: usize) -> []u8 {
    const buf: anyptr = stackalloc(max_len)
    return cast(buf, []u8, max_len)
}
```

The memory is only valid for the lifetime of the stack frame that allocated it — never return a pointer/slice derived from `stackalloc` out of the function that called it. Each call to `stackalloc` (e.g. inside a loop body) allocates a fresh region for that iteration; the storage is not hoisted and reused across iterations.

### `match` Expression

```mirage
match operand {
    pattern1: expr1,
    pattern2: expr2,
    _: default_expr
}
```

`match` is an expression — all arms must produce the same type. See [Match and Switch](#8-match-and-switch) for pattern details.

### `try` Expression

```mirage
try fallible_call(args)
```

Calls a fallible function (one whose last return type is `error(...)`, see
§16). If the call's error result is in the `Failed` state, propagates it by
returning from the enclosing function; all deferred statements in scope run
before the return. On success, evaluates to the non-error return values, and
the callee's error result is discarded (there is nothing left to check — it
is `Ok`).

`try` requires the callee's `error(...)` type to be a subset of (or equal
to) the enclosing function's own `error(...)` type — every error member type
the callee can produce must also be a member of the caller's declared union.

### Braced Initializers

```mirage
{.field = val, .field2 = val2} // struct initializer
{val1, val2, val3}             // array initializer
{}                             // empty (full default initialization)
```

An array initializer's last value may end with `...` to fill all remaining elements with that same value (evaluated once):

```mirage
mut npc_ids: [10]i32 = { -1... }              // all 10 elements set to -1
const levels: [10]i32 = { 0, 1, 2, 3, 4, 5... } // 0, 1, 2, 3, 4, then five more 5s
```

### Import Expression

```mirage
const mod := import("path/to/module")
```

`import(...)` is only valid as the initializer of a `const` declaration with no explicit type. It binds the imported module as a namespace.

A chain of plain `.field` accesses may follow `import(...)` directly, as a shortcut for pulling a
single value out of a module without binding the module itself to a name:

```mirage
const target_arch := import("path/to/module").target_arch
```

This is equivalent to the two-step form (`const mod := import(...)` then `mod.target_arch`
elsewhere). Only `.field` accesses are accepted in the chain — never a call or index — since the
`import(...)` result itself remains a namespace binding, not a value.

### `import_bin` Expression

```mirage
const font_data := import_bin("assets/font.ttf")
```

`import_bin` embeds the raw bytes of a file into the compiled binary at compile time, as a constant array of type `[N]u8` where `N` is the file's size in bytes. Unlike `import(...)`, the result is an ordinary array value — it can be used anywhere a `[N]u8` constant is valid, not just as a whole `const` initializer.

The path is a string literal resolved **relative to the directory of the importing module** (not the current working directory). Rules enforced at compile time:
- The resolved path must stay inside the module's own directory — using `../` to escape it is a sema error.
- A missing file is a sema error.
- A file that cannot be read is a sema error.
- A file larger than 1 MiB compiles successfully but emits a sema **warning**, since embedding very large files bloats the executable.

`import_bin` is a compile-time constant expression, so its size is known to the compiler — the resulting binding's type is always a fixed-size array, e.g. `const icon := import_bin("assets/icon.bin")` gives `icon` the type `[N]u8` for whatever `N` the file's byte length is.

---

## 6. Statements

### Block Statement

```mirage
{
    stmt1
    stmt2
}
```

Introduces a new scope. Variables declared inside are not visible outside.

### If / Else

```mirage
if condition {
    // then
} else if other_condition {
    // else if
} else {
    // else
}
```

The `else` branch is optional. The condition must be `bool`. The body can be any statement (not necessarily a block).

### `when` Statement

```mirage
when condition {
    // then — must be a compile-time constant expression
} else when other_condition {
    ...
} else {
    ...
}
```

A compile-time-only sibling of `if`: `condition` must be a compile-time
constant (a sema error otherwise, pointing at `if` instead); the `then`
branch (and each block in an `else`/`else when` chain) must be a literal
block, unlike `if`'s. Both branches are always type-checked, but only the
selected one is ever emitted. See
[Compile-Time Configuration](#12-compile-time-configuration) for the full
semantics, including the additional restrictions that apply at module
scope.

### While Loop

```mirage
while condition {
    // body
}
```

Loops while `condition` is `true`. The condition is `bool`. The body must be a block statement. `break` exits the loop; `continue` jumps to the next iteration.

### For Loop

```mirage
for x in 0..10 { ... }        // range, exclusive upper bound
for x in ..10 { ... }         // range with implicit lower bound of 0
for x in some_slice { ... }   // element by value
for i, x in some_slice { ... }  // index + element by value
for &x in some_slice { ... }    // element by reference (*T)
for i, &x in some_slice { ... } // index + element by reference
```

Iterates a range (`lower..upper`, exclusive of `upper`), or a slice/fixed-size array. With a single
binding, only the element is bound; with two bindings, the first is the `usize` index and the
second is the element. Prefixing the element binding with `&` binds a pointer to the element
in-place (mutating through it mutates the underlying slice/array) instead of a by-value copy. Any
binding name may be `_` to discard it. `break` and `continue` behave as in `while`.

### Break and Continue

```mirage
break
continue
```

`break` exits the nearest enclosing loop. `continue` jumps to the top of the nearest enclosing loop. Both run deferred statements registered in the loop body scope before transferring control.

### Return

```mirage
return
return value
return val1, val2    // multi-return
```

Returns from the current function. For multi-return functions, multiple values are returned separated by commas. Deferred statements run before the actual return.

### `return_err` and `return_ok`

```mirage
return_err .Variant
return_err .Variant(payload)
return_err TypeName.Variant{.field = value}
return_ok [value1, value2, ...]
```

Sugar over `return` for fallible functions (functions whose last return type
is `error(...)`, see §16). Both run deferred statements before returning,
exactly like `return`.

`return_err <expr>` returns from the current function with the error
result; all non-error return slots are `undefined` (matching what `try`
emits on its error-propagation path — the two lower identically). `<expr>`
may take any of these forms:

- `.Variant` / `.Variant(payload)` / `TypeName.Variant{.field = value}` —
  naming a variant of one of the function's declared `error(...)` member
  types. The result is `Failed`, carrying the constructed variant as the
  payload.
- An arbitrary expression whose type is already one of the function's
  declared `error(...)` member types (e.g. a variable holding a bare enum
  or tagged-union error value). The result is `Failed`, carrying that value
  as the payload — equivalent to the variant-sugar forms above but without
  requiring `.Variant` syntax at the call site.
- An arbitrary expression whose type is itself an `error(...)` value —
  this function's own, or (like `try`) one whose member types are a subset
  of this function's — propagated as-is, `Ok` or `Failed` alike, with
  cross-union tag translation applied automatically when the two
  `error(...)` types differ.

```mirage
pub type MemoryError = enum(i32) {
    OutOfMemory = 1
}

fn alloc(n: usize) -> (anyptr, error(MemoryError)) {
    if n == 0 {
        return_err .OutOfMemory
    }
    ...
}

fn realloc(n: usize) -> (anyptr, error(MemoryError)) {
    const p, err := alloc(n)
    if err {
        // 'err' is already an error(MemoryError) value; propagated as-is.
        return_err err
    }
    ...
}
```

Unqualified `.Variant` / `.Variant(payload)` is accepted when the variant
name is unique across all of the function's error member types; qualify as
`TypeName.Variant{...}` when it isn't (this qualified form currently only
supports variants with a payload — an ambiguous payload-free variant name
must be renamed to disambiguate). The leading dot is always required for
the unqualified form.

Sema errors:
- `return_err` used in a function whose last return type is not
  `error(...)`.
- The named variant does not belong to any of the function's error member
  types.
- The variant name is ambiguous across member types and was not qualified.
- The operand is itself an `error(...)` value whose member types are not a
  subset of the function's declared `error(...)` type.
- The operand is none of the above (not a `.Variant` form, not a value of
  one of the function's error member types, and not a compatible
  `error(...)` value).

`return_ok [expr {, expr}]` returns from the current function with the
error result in the `Ok` state. The operands supply the non-error return
values in order, matching the function's non-error return types exactly via
the same checking rules as `return`.

```mirage
return_ok named_type          // -> (*ast.NamedType, error(E))
return_ok a, b                // -> (T1, T2, error(E))
return_ok                     // -> error(E)  (bare error-only return)
```

Sema errors:
- `return_ok` used in a function whose last return type is not
  `error(...)` (including non-fallible functions).
- Wrong number of non-error values supplied.
- Type mismatch on any supplied value (same checks as `return`).

### Defer

```mirage
defer stmt
defer { stmts... }
```

Registers `stmt` to run when the enclosing block exits (whether by fall-through, `return`, `break`, or `continue`). Multiple defers in the same scope run in LIFO (last-in, first-out) order. Each loop iteration has its own defer scope.

Restrictions:
- `try` inside a defer body is a sema error
- `return`, `break`, or `continue` that escape a defer body are sema errors

### Switch Statement

```mirage
switch operand {
    pattern1: stmt1,
    pattern2: stmt2,
    _: default_stmt
}
```

Statement-level counterpart to `match`. No exhaustiveness requirement. Arm bodies are statements (not expressions). `break`/`continue` inside arms bind to the enclosing loop, not the switch. No fallthrough. See [Match and Switch](#8-match-and-switch) for pattern details.

### Variable Declaration Statement

```mirage
mut x: i32 = 5
mut x := 5
mut x: i32           // default-initialized
const y := 10
mut a, b := func()   // group declaration
```

---

## 7. Functions

### Regular Functions

```mirage
fn name(param1: Type1, mut param2: Type2) -> ReturnType {
    // body
}

pub fn name(p: Type) -> (T1, T2) {
    // multi-return
}
```

- Parameters are immutable by default; `mut` makes a parameter mutable.
- `pub` makes the function visible to importing modules.
- Multi-return: `-> (T1, T2, ...)` syntax.
- Void return: omit the `->` clause.

### Default Parameter Values

```mirage
fn alloc(size: usize, zero_memory: bool = true) -> (anyptr, Allocator_Error)
fn repeat(s: []u8, times := 3) -> []u8   // inferred type form
```

A parameter may declare a default value, used at call sites where the
caller omits that argument (and every argument after it):

```mirage
const p1 := try alloc(1024)          // zero_memory = true
const p2 := try alloc(1024, false)   // zero_memory = false

const s1 := repeat("hi")             // times = 3
const s2 := repeat("hi", 5)          // times = 5
```

- **Two forms**: `name: Type = expr` declares the type explicitly; `name :=
  expr` infers the parameter's type from the default expression, using the
  same literal-defaulting rules as an inferred-type `mut`/`const` variable
  declaration.
- **Ordering**: once any parameter in a list has a default value, every
  parameter after it must also have one. `self` in a method declaration is
  exempt — it's never passed explicitly at a call site, so it isn't subject
  to this rule.
- **Variadic exclusion**: a native variadic parameter (`...T`) cannot
  appear in the same parameter list as a defaulted parameter.
- **Scope**: a default expression is evaluated at the call site but checked
  in *module* scope, not the function's own local scope — it may reference
  `const` declarations, imported module symbols, global `mut` variables,
  and other functions, but it can never refer to another parameter of the
  same function (parameters are simply not in scope for this check).
- **No `try`**: a default expression may not contain `try` — it's checked
  once, at the function's declaration site, with no calling function's
  error type or defer state available yet to check it against.
- **Evaluation**: a compile-time-constant default (`= true`, `= 0`, an enum
  literal, ...) is folded directly into the call site with no runtime
  code. A non-constant default is evaluated inline at the call site,
  exactly once per call that omits the argument, in the same position a
  literal argument expression would occupy.
- **Only at the call site**: calling through a function pointer always
  requires every argument explicitly — a function pointer's type carries no
  default-value information (two functions with identical signatures but
  different defaults share the same function-pointer type), and the call
  site has no way to know which concrete function it will end up calling.
  Omitting an argument through a function pointer is an ordinary arity
  error.
- Inherent methods (`impl Type { ... }`) may declare their own defaults,
  following the same rules as free functions. Trait methods and their
  implementations have additional rules — see
  [Traits and Dynamic Dispatch](#10-traits-and-dynamic-dispatch).

### Native Variadic Parameters

```mirage
fn sum(base: i32, nums: ...i32) -> i32 {
    mut total := base
    for n in nums {
        total += n
    }
    return total
}

sum(10)          // zero variadic args -> nums is an empty slice
sum(10, 1, 2, 3) // nums is []i32{1, 2, 3}
```

The final parameter of a `fn` may be declared `name: ...T`, where `T` is any valid element type.
Inside the function body, `name` behaves as an ordinary `[]i32` — a value of `[N]T` collected from
the trailing call arguments, or the slice passed directly via [spread](#function-call). A call must
supply zero or more trailing arguments assignable to `T` beyond the fixed parameters. This is
distinct from `ext fn`'s untyped C `...` varargs (see [Variadic Arguments](#17-variadic-arguments))
— the address of a variadic function cannot be taken as a function pointer.

### Extern Functions

```mirage
ext fn puts(s: *u8) -> i32
ext fn printf(fmt: *u8, ...) -> i32   // variadic
pub ext fn malloc(size: usize) -> anyptr
```

Declares an external C function. `ext fn` functions:
- Accept at most one `...` at the end (variadic)
- `...` requires at least one named parameter before it
- Return at most one type
- Parameters and return types may be scalars, pointers, function pointer types, structs, arrays, or
  slices — a struct crossing the boundary is passed/returned using the platform C ABI (e.g. small
  structs are packed into registers on x86-64, matching a C compiler's own calling convention for
  that struct)
- Cannot have parameters or return types that are `union`s (tagged or untagged — not yet supported
  across an `ext fn` boundary) or trait handles (no C ABI representation); a multi-return function
  pointer type is also rejected (no C ABI representation for multiple return values)
- Cannot declare [default parameter values](#default-parameter-values) — a
  default argument has no C ABI representation, so this is a sema error at
  the declaration site

### Entry Points

The root module must define one of:
```mirage
pub fn main()               // void; exits with code 0
pub fn main() -> i32        // exits with this code
pub fn main() -> error(E)   // exits 0 on Ok, 1 on Failed
```

For freestanding builds (`--freestanding`), use `fn _start()` instead.

### Function Pointers

```mirage
mut fp: fn(i32, i32) -> i32 = add
fp = mul
fp(3, 4)
```

See [Function Pointer Types](#function-pointer-types) for the type syntax.

### Macros

```mirage
macro name(param1: Type1, param2: Type2) -> expr
macro name(param1: Type1): ReturnType -> expr
pub macro align_up(n: usize) -> (n + (alignment - 1)) & ~(alignment - 1)
pub macro new_vector(element_size: usize): Vector -> { .element_size = element_size }
```

Macros are expression-level compile-time substitutions. They are called with the same syntax as functions. Parameters are typed. The body is an expression template.

An optional `: Type` annotation between the parameter list and `->` declares the macro's result type explicitly. It's required when the body's type can't be inferred without context — for example a struct-literal body, which needs an expected type the same way a `const`/`mut` initializer does. When present, the body is checked against the declared type and a mismatch is reported on the macro declaration. When absent, the result type is inferred from the body, as before.

### Declaration Attributes

A `fn` declaration may be preceded by an `@name` attribute clause (`@naked`, `@no_return`,
`@always_inline`, `@section("...")`, `@init`) — see [Declaration Attributes](#21-declaration-attributes)
for the full syntax and semantics.

---

## 8. Match and Switch

Both `match` (expression) and `switch` (statement) use the same arm pattern syntax.

### Arm Patterns

**Variant pattern** (for enum and tagged union operands):
```mirage
.field_name
.variant_name(capture)     // binds payload struct by value
.variant_name(&capture)    // binds payload struct by reference (*PayloadType)
```

**Literal pattern** (for integer and bool operands):
```mirage
42
-1
true
false
MY_CONSTANT
```

Literal patterns must be compile-time constant expressions. Duplicate literal values in the same match/switch are a sema error.

**Default pattern**:
```mirage
_
```

Matches any value. Must be the last arm. At most one `_` allowed per match/switch.

### Match Exhaustiveness

**Enum operand:** All variants must be covered, OR a `_` default must be present. A `_` after all variants are already covered is an error.

**Bool operand:** Both `true` and `false` must be covered, OR a `_` must be present.

**Integer operands:** A `_` default is required (exhaustiveness cannot be verified).

**Tagged union operand:** Same rules as enum — all variants must be covered or `_` present.

### Switch vs Match

`switch` is a statement: arm bodies are statements, no result type, no exhaustiveness check. `match` is an expression: arm bodies are expressions, all arms must produce the same type, exhaustiveness is checked.

---

## 9. Impl Blocks and Methods

```mirage
impl TypeName {
    fn method_name(self) -> ReturnType {
        // access self.field
    }

    pub fn mutable_method(mut self, arg: i32) -> i32 {
        self.field += arg
        return self.field
    }
}
```

- Methods are associated functions on a named type (struct or enum).
- The first parameter is always `self` or `mut self`.
- `self` is internally a pointer (`*T`); field access and method calls auto-deref.
- `mut self` allows mutation of the receiver's fields.
- `pub` on individual methods makes them visible cross-module.
- `impl` blocks cannot be `pub` (the individual methods control visibility).
- Methods are called as `value.method(args)` or `pointer.method(args)`.
- Cross-module: `module_name.TypeName` struct can have methods defined in the type's own module.
- Non-`self` parameters may declare [default parameter values](#default-parameter-values), following the same rules as free functions.

---

## 10. Traits and Dynamic Dispatch

Mirage has no generics. Traits exist for exactly one purpose: dynamic
dispatch through a uniform handle type — the same model as Go interfaces or
Rust `dyn Trait`. There is no compile-time monomorphization and no type
parameters.

### Declaring a Trait

```mirage
type Drawable = trait {
    fn draw(self)
    fn bounding_box(self) -> (i32, i32, i32, i32)
}
```

A trait is declared like any other named type: `type Name = trait { ... }`.
Its body is a list of method signatures — `fn` declarations with `self` or
`mut self` as the first parameter, optional further parameters, and optional
return types, but **no body**. A trait must declare at least one method (an
empty `trait { }` is an error). `pub` is not allowed on an individual trait
method declaration — the trait's own `pub` (or lack of it) governs whether
importing modules can use it at all. Native-variadic (`...T`) trait method
parameters are rejected: there is no vtable-entry representation for a
variadic call.

A trait method may declare [default parameter values](#default-parameter-values).
Every implementation of that method — through any `impl TRAIT for TYPE`
block, and (as of dynamic dispatch) any call through a `dyn Trait` handle —
inherits that default; see [Implementing a Trait](#implementing-a-trait)
below for how an impl must (not) redeclare it.

**Using a trait name in type position denotes a HANDLE, not the trait
definition.** Semantically this behaves like Go's interface types or Rust's
`dyn Trait`: it means "any value that implements this trait," and the
compiler only accepts a pointer to a conforming type there. Concretely, a
handle is a fat pointer — a data pointer plus a vtable pointer — always 16
bytes, 8-byte aligned, regardless of which trait it names. This is why only
a pointer (not a bare value) coerces to it, and why handles have no `ext fn`
ABI representation.

### Implementing a Trait

```mirage
type Circle = struct { x: i32; y: i32; r: i32 }

impl Drawable for Circle {
    fn draw(self) { // draw the circle }
    fn bounding_box(self) -> (i32, i32, i32, i32) {
        return self.x - self.r, self.y - self.r,
               self.x + self.r, self.y + self.r
    }
}
```

`impl TRAIT for TYPE { ... }` implements a trait for a concrete type — TYPE
must be a named type (struct, enum, union, or type alias), not a raw pointer
type; `self` inside the impl is a pointer to TYPE, exactly like a bare
`impl TYPE { ... }` block. `pub` is not allowed on individual methods inside
a trait impl, for the same reason it's disallowed inside the trait
declaration itself.

**Conformance**: every method the trait declares must be implemented in the
`impl TRAIT for TYPE` block, with an exactly matching signature (same name,
same `self`/`mut self`, same parameter types, same return types — default
values are not part of this comparison, see below). A trait impl may not
contain methods beyond the trait's own surface — put those in a separate
bare `impl TYPE { }` block instead.

**Default parameter values are never redeclared in an impl.** If the trait
method declares a default for a parameter, the impl's method must declare
that parameter *without* a default — it's inherited automatically:

```mirage
type Allocator = trait {
    fn alloc(self, size: usize, zero_memory: bool = true) -> (anyptr, Allocator_Error)
}

impl Allocator for MyAllocator {
    fn alloc(self, size: usize, zero_memory: bool) -> (anyptr, Allocator_Error) {
        // implementation
    }
}
```

Declaring a default in the impl that the trait method already has is a sema
error ("redeclares a default value already declared by the trait"); declaring
a default the trait method does *not* have is also a sema error ("defaults on
trait implementations must match the trait declaration"). The default used
to fill an omitted argument always comes from the trait's own method
signature, never from any impl — this holds for a call through a concrete
type's own method just as much as for a call through a `dyn Trait` handle,
since an impl is never allowed to carry a default of its own to begin with.

**Coherence**: an `impl TRAIT for TYPE` is only legal in the module that
defines TRAIT or the module that defines TYPE. Implementing someone else's
trait for someone else's type (an "orphan impl") is an error. A given
`(TRAIT, TYPE)` pair may be implemented at most once anywhere in the
program.

**Method name collisions** are resolved at impl-declaration time, never at a
call site: a trait-impl method with the same name as an existing bare-impl
method on the same type is an error at the trait impl; two different trait
impls on the same type supplying a method of the same name is an error at
the second one. Every valid program is therefore statically unambiguous.

### Static and Dynamic Dispatch

Trait methods are callable directly on a concrete type or a pointer to one —
this is **static dispatch**, resolved at compile time with no vtable
involved:

```mirage
mut circle: Circle = { .x = 10, .y = 10, .r = 5 }
circle.draw()          // static dispatch: calls Circle's implementation directly
```

Method-call resolution on a concrete receiver checks the type's own bare
`impl` block first, then its trait impls.

A pointer to a type that implements a trait **coerces to that trait's
handle** wherever an expected type is known (variable initializers,
assignment, return statements, call arguments, struct/array field
initializers) — the same contextual mechanism used for `default`,
`undefined`, and implicit tagged-union wrapping elsewhere in the language.
The source must be a pointer; coercing a bare (non-pointer) value is an
error, as is coercing a pointer to a type that doesn't implement the trait.

```mirage
mut shapes: [2]Drawable = { &circle, &rect }   // &circle, &rect coerce to Drawable handles

for shape in shapes {
    shape.draw()        // dynamic dispatch: resolved through the handle's vtable at runtime
}
```

Calling a method through a handle is **dynamic dispatch**: it resolves
against the trait's own method list and dispatches through the handle's
vtable at runtime. Both static and dynamic dispatch ultimately call the
exact same underlying function — the vtable exists only so the call site
doesn't need to know the concrete type.

`try` on a fallible trait method works identically whether the call is
static or dynamic. A multi-return trait method can be captured with a group
declaration through a handle just like any other multi-return call.

An omitted trailing argument on a defaulted trait-method parameter is
resolved **before** the vtable call — the caller fills in the trait's
default value itself, and the vtable slot always receives the full argument
list. This is true for both static and dynamic dispatch, so it doesn't
matter whether the compiler happens to know the concrete type behind the
call.

### Handle Values

`nil` is assignable to a handle (both the data pointer and the vtable
pointer are zero). `default` for a handle type is `nil`. `undefined` is
legal (uninitialized storage, no zeroing). **Calling through a nil handle is
undefined behavior** — no runtime check is emitted; it will crash.

A handle supports no field access, no dereference, and no arithmetic. The
only comparisons allowed are `==` and `!=` against `nil` — comparing two
non-nil handles for equality, or attempting any relational (`<`, `>`, etc.)
or arithmetic operator, is an error.

Handles round-trip like any other 16-byte value: they can be struct fields,
function parameters, return values, and array/slice elements, with no
special handling beyond their size and layout.

**Handles cannot appear in `ext fn` signatures** — a handle has no C ABI
representation.

**There is no downcasting.** Once a concrete pointer is coerced to a handle,
there is no way to recover the concrete type or pointer from the handle.

---

## 11. Modules

### Importing a Module

```mirage
const io := import("path/to/module")
```

`import(...)` is valid only as the initializer of a `const` declaration with no explicit type. The result is a namespace binding, not a value — it cannot be assigned, passed as an argument, or stored.

A chain of plain `.field` accesses is accepted directly after `import(...)` — no call or index —
as a shortcut for extracting one value without a separate binding for the module itself:

```mirage
const target_arch := import("path/to/module").target_arch
```

See [Import Expression](#import-expression) and [Compile-Time Configuration](#12-compile-time-configuration) for a full example.

The path is resolved in two steps:
1. **Relative to the importing module's own directory** — `<importing-module-dir>/<path>` — if that directory exists, it's used.
2. Otherwise, relative to the directory named by the **`MIRAGE_PATH` environment variable**, if set. The resolved path must stay inside `MIRAGE_PATH` (no escaping it with `../`); if it doesn't resolve to a directory inside `MIRAGE_PATH`, the import fails.

This lets a project keep local, closely-related modules as subdirectories of the importing module, while sharing a common library root via `MIRAGE_PATH` for anything meant to be reused across unrelated parts of a project.

The `mirage build`/`mirage run` CLI also accepts a `--std=<path>` flag, which overrides `MIRAGE_PATH` for that invocation.

### Accessing Module Symbols

```mirage
io.print("hello")
const x: io.SomeType = io.some_value
```

Cross-module access uses dot notation. Only `pub` symbols are accessible from outside their defining module.

### Visibility

- Top-level declarations without `pub` are module-private.
- `pub fn`, `pub type`, `pub mut`, `pub const` are accessible to importing modules.
- `impl` blocks: the block itself is not pub; use `pub` on individual method declarations.

### Bare Import

```mirage
import("path/to/module")
```

A second, standalone form of `import(...)`: a bare module-scope *declaration*, as
opposed to the bound form above (`const mod := import(...)`), which binds the
target module to a namespace. A bare import instead makes every `pub` symbol of
the target module — functions, types, `const`/`mut` globals, macros, and `ext fn`
declarations — available in the current module as **private, unqualified local
names**, as if each had been declared locally:

```mirage
// core/math.mir
pub fn abs(x: i32) -> i32 { return x < 0 ? -x : x }
pub fn max(a: i32, b: i32) -> i32 { return a > b ? a : b }
pub const PI: f64 = 3.14159265358979

// main.mir
import("core/math")

pub fn main() -> error {
    const x := abs(-5)     // resolves to core/math.abs
    const m := max(x, 10)  // resolves to core/math.max
    const pi := PI         // resolves to core/math.PI
    return_ok
}
```

**No namespace binding is created** — unlike the bound form, `import("core/math")`
does not introduce a name like `math`. `PI` is reachable as `PI`, never as
`math.PI`; to also use qualified access, add a bound import alongside (see
"Bare and bound together," below).

**Restrictions**:
- Legal only at module scope (top-level declaration position), including inside a
  module-scope `when {}` block only if that block's own allow-list permits it —
  it currently doesn't (see [Compile-Time Configuration](#12-compile-time-configuration)),
  so a bare import inside a module-scope `when {}` block is a sema error.
  Everywhere else a statement is legal — a function body, a `when {}` block at
  function scope — a bare `import(...)` is a **parse** error, not a sema error:
  ```
  error: bare 'import(...)' is a module-scope declaration, not a statement.
         To use symbols from another module inline, bind it:
         'const mod := import("path")'
  ```
- `pub` is never legal on a bare import — a parse error:
  ```
  error: bare imports cannot be 'pub'. All imported symbols are private
         to this module and cannot be re-exported.
  ```
  Every symbol it introduces is therefore private to the importing module,
  regardless of the original's own visibility, and can never be re-exported.

**What gets imported**: every symbol declared `pub` in the target module — `fn`,
`type`, `const`/`mut`, `macro`, `ext fn`. `impl` blocks are never imported: method
resolution on a value of a bare-imported type already works through the type's
own defining module, with no extra step needed. Non-`pub` symbols are not
imported — using one produces an ordinary "undefined identifier" error, not a
"not pub" error, since it was never made visible in the first place.

**Collisions**: registering an imported name follows the same rule as any other
module-scope name — the first declaration wins. Two shapes of collision can occur:
- **Against a local declaration** (in either order — the local declared before or
  after the bare import): an ordinary redefinition error.
- **Against a name already introduced by an earlier bare import in the same
  module**: reported at the *second* bare import's own declaration site, naming
  both source modules and suggesting a bound-import disambiguation:
  ```
  error: bare import of 'core/net' introduces symbol 'open' which
         conflicts with 'open' already imported from 'core/os'.
         Use bound imports to disambiguate:
           const os  := import("core/os")
           const net := import("core/net")
           os.open(...)  net.open(...)
  ```

**Non-transitive**: if `core/os` itself uses a bare import to pull symbols from
`core/io` into its own private namespace, those symbols stay private to `core/os`
— `import("core/os")` elsewhere does not also expose them. Only what is
explicitly `pub` in `core/os` itself propagates.

**Bare and bound together**: importing the same module both ways is legal and
does not conflict by itself — the bound form introduces a namespace binding
(`os`), the bare form introduces individual unqualified aliases (`open`, `close`,
...), and these live in different senses even though they share one symbol
table:

```mirage
import("core/os")             // bare: unqualified names
const os := import("core/os") // bound: the 'os' namespace, same module

open(...)      // via the bare import
os.open(...)   // via the bound import
```

The ordinary collision rule still applies to any individual bare-imported name
that happens to collide with anything else in the module — including the bound
import's own namespace name, if one happens to coincide.

---

## 12. Compile-Time Configuration

Seven coupled features let a module read compile-time-supplied
configuration, conditionally compile declarations, statements, and linker
inputs based on it, and surface configuration problems as sema
diagnostics: `#option`, `#env`, `#link`, `#error`, `#warn`, the `when`
statement, and the `when` expression.

### `#option`

```mirage
#option(key)
#option(key, default)
```

A compile-time expression that reads a value supplied by the compiler
driver via `--opt key=value` (repeatable). `key` is a string literal (by
convention a `/`-separated path, e.g. `"build/target_os"`). If a second
argument is given, it is used when `--opt key=...` was not passed; if no
default is given and no `--opt` is passed, this is a sema error:

```
error: required option 'build/target_os' was not provided.
       Pass it with: --opt build/target_os=<value>
```

`#option` is legal anywhere an expression is legal — nested inside
arithmetic (`#option(...) + 1`), passed as a function argument (including
`#link`'s `data` argument), as a `mut` variable's initializer, and so on.
Its value is always resolved once from `--opt`/the default and cached, so it
behaves as an ordinary compile-time-constant expression wherever it appears.

**Target-type resolution**, in priority order:
1. The expected type from context (e.g. the declared type of a `const`).
2. If no expected type is known but a default value is given, the default
   value's own type (via the usual literal-defaulting rules).
3. Otherwise, `[]u8` — the raw `--opt` string is used as-is.

**`--opt` value coercion**, once the target type is known:
- `bool`: `"true"`/`"1"` → `true`; `"false"`/`"0"` → `false` (case-insensitive). Anything else is a sema error.
- Integer types (`i32`, `u32`, `usize`, ...): parsed as a decimal integer. Out-of-range or non-numeric is a sema error.
- `[]u8`: the raw string, unconverted.
- Enum types: matched first by variant name (case-sensitive), then by integer value (e.g. `--opt build/target_os=Windows` and `--opt build/target_os=1` can both select the same variant). Neither matching is a sema error naming the valid variant names.
- Any other target type: a sema error — `#option` does not support it.

### `#env`

```mirage
#env(key)
#env(key, default)
```

Identical to `#option` in every respect — legality, target-type resolution,
value coercion, and the required-value error shape — except that `key`
names an **environment variable** instead of a `--opt` key, and the value
comes from `std::getenv(key)` (read once at compile time, on the machine
running the compiler) instead of `--opt key=value`:

```
error: required environment variable 'MIRAGE_TARGET_ARCH' was not set.
       Set it with: MIRAGE_TARGET_ARCH=<value>
```

```mirage
pub const target_os:   OperatingSystem = #env("MIRAGE_TARGET_OS",   .Linux)
pub const target_arch: Architecture    = #env("MIRAGE_TARGET_ARCH", .X86_64)
```

### `#link`

```mirage
#link(category, data)
```

`category` is one of `lib`, `system`, or `flag` (bare identifiers, not
string literals): `lib` links a library file (`data` is a path, relative to
the directory of the current module file); `system` links a system library
by name (searched in the linker's default paths); `flag` passes `data`
verbatim as a raw linker flag. `data` must be a compile-time constant
`[]u8` expression — a `when` expression is legal here:

```mirage
#link(lib, "raylibdll.lib" when raylib_shared else "raylib.lib")
```

`#link` is legal only at module scope, or inside a module-scope `when`
block; anywhere else (a function body, an `if`, etc.) it is a sema error:

```
error: '#link' is a linker directive and may only appear at module scope
       or inside a module-scope 'when' block.
```

The compiler collects every `#link` directive reachable from a *live*
`when` branch (see below), across every compiled module, into a single
list the driver can act on (the exact linking mechanism is driver-specific
and out of the compiler's scope — collection only).

### `#error` / `#warn`

```mirage
#error(message)
#warn(message)
```

Compile-time diagnostic directives: `#error` emits a sema **error** at the
directive's location; `#warn` emits a sema **warning**. `message` must be a
compile-time constant `[]u8` expression, exactly like `#link`'s `data`
argument. On their own, both are unconditional — `#error(...)` reached
anywhere in a live position always fails the build. Their real use is
inside a `when` block, to reject or flag configurations at the point
they're selected instead of failing later with a less specific error:

```mirage
when target_os == .Windows {
    #error("Windows is not supported")
} else when target_arch == .Wasm32 {
    #warn("Wasm32 support is experimental")
}
```

Like `#link`, `#error`/`#warn` are legal only at module scope, or inside a
module-scope `when` block; anywhere else (a function body, an `if`, etc.)
it is a sema error:

```
error: '#error' is a compile-time diagnostic directive and may only appear
       at module scope or inside a module-scope 'when' block.
```

The **unselected** branch of a `when` still fully type-checks an
`#error`/`#warn` directive's `message` argument (per the "both branches
type-checked" rule below) — it just never actually emits the diagnostic.

### `when` Statement

```mirage
when condition {
    ...
} else when other_condition {
    ...
} else {
    ...
}
```

A compile-time conditional statement, legal anywhere an ordinary statement
is legal (function bodies, module scope, nested inside other `when`/`if`/
`while` blocks). `condition` must be a compile-time constant expression —
a runtime value is a sema error pointing at `if` instead:

```
error: 'when' condition must be a compile-time constant expression.
       Use 'if' for runtime conditions.
```

**Both branches are always type-checked** — a hard requirement, with no
opt-out. This is deliberate: it prevents platform-specific code from
silently rotting when developing on a different target than the one a
given `when` branch targets. Only the *selected* branch's code is ever
emitted by codegen; the other branch is never executed and never even
visited by codegen (an `ext fn` declared only in an unselected module-scope
branch, for instance, never needs an LLVM declaration synthesized for it).

**At module scope**, a `when` block may contain only `#link`, `#error`, and
`#warn` declarations, `const` declarations initialized directly with
`#option`/`#env`, `type` declarations, and `ext fn` declarations — anything
else is a sema error:

```
error: only '#link', '#error', '#warn', 'const' with '#option'/'#env',
       'type', and 'ext fn' declarations are permitted inside a
       module-scope 'when' block.
```

This is what makes platform-specific `ext fn` declarations possible:

```mirage
when target_os == .Windows {
    ext fn GetLastError() -> u32
}
```

A name declared only inside a module-scope `when`'s selected branch is an
ordinary symbol wherever it's visible; a name declared only in the
*unselected* branch simply doesn't exist — referencing it (even from the
other branch of the same `when`) is an ordinary "unknown identifier" error,
not a special diagnostic. `#link` is collected only from the selected
branch; the unselected branch's `#link` `data` expression is still fully
type-checked (per the "both branches type-checked" rule above), just never
collected.

### `when` Expression

See [`when` Expression](#when-expression) under Expressions — same
construct, used here as `#link`'s `data` argument or an `#option`/`#env`
default/operand.

### Example

```mirage
// Core/Compiler/Options/main.mir
const target_os   := import("Core/Compiler/Options").target_os     // chained '.field' shortcut
const target_arch := import("Core/Compiler/Options").target_arch   // (see Import Expression)

const raylib_shared := #option("raylib_shared", false)

when target_os == .Windows {
    #link(lib, "windows/raylibdll.lib" when raylib_shared else "windows/raylib.lib")
    #link(system, "User32")
} else when target_os == .Linux {
    #link(lib, "linux/libraylib.so.600" when raylib_shared else "linux/libraylib.a")
    #link(system, "dl")
} else {
    #link(lib, "raylib")
}

ext fn InitWindow(width: i32, height: i32, title: *u8)
```

---

## 13. Type Declarations

```mirage
type Name = TypeExpression
pub type Name = TypeExpression
```

Creates a named type alias. The named type is structural: two declarations with the same structure resolve to the same underlying type.

---

## 14. Type Inference

- `mut x := expr` infers the type of `x` from `expr`.
- `const x := expr` infers the type from `expr`.
- `name := expr` on a function/method parameter infers the parameter's type from its default expression `expr`, the same way — see [Default Parameter Values](#default-parameter-values).
- Function parameter types, return types, and `const`/`mut` with an explicit type annotation always resolve exactly.
- `default`, `undefined`, `.field` enum literals, and braced initializers require an expected type (from annotation or context) to be set.
- When calling a function that takes a known type, argument expressions are type-checked against that expected type.

---

## 15. Type Compatibility and Assignability

The following types are mutually assignable without explicit cast:

| From            | To            | Notes                                |
|-----------------|---------------|--------------------------------------|
| `anyptr`        | `*T`          | unsafe, no check                     |
| `*T`            | `anyptr`      |                                      |
| `anyptr`        | fn ptr type   | nil-to-fn-ptr coercion               |
| fn ptr type     | `anyptr`      |                                      |
| `[N]T`          | `[]T`         | array decays to slice                |
| `bitset(...)`   | its storage type | expected-type position only — see §18, "Bitset Types" |

Arithmetic, bitwise, and other binary operations require both operands to have the same type (except `anyptr ± integer`). The bitset → storage-type row above is deliberately **not** symmetric and does **not** apply during binary-operator type resolution: a raw integer never implicitly coerces to a bitset (use `cast`), and mixing a bitset with a raw integer in a binary operator is always a sema error regardless of expected type.

---

## 16. Error Type System

Errors are typed enum or tagged-union values, wrapped in a
compiler-generated `Ok`/`Failed` tag. A fallible function declares its
error type explicitly in its return signature — there is no untyped
"error code" convention.

### Declaring Error Types

Any `enum(i32)` or `union(enum)` type becomes an error type by virtue of
being used inside an `error(...)` return type — no special keyword or
attribute is required:

```mirage
pub type MemoryError = enum(i32) {
    OutOfMemory = 1
    NotFound    = 2
}

pub type IoError = union(enum) {
    NotFound:  []u8   // carries the path that wasn't found
    Timeout:   u32     // carries the timeout value that elapsed
    Closed             // no payload
}
```

Any variant value is permitted, including `0` — there is no zero-value
restriction (success/failure is tracked by the separate `Ok`/`Failed` tag,
not by the payload value). An `enum(i32)` error type's underlying type
must be `i32`.

### Fallible Functions

A function is fallible when its last return type is `error(T)` (a single
error type) or `error(A | B | C)` (a union of distinct error types):

```mirage
pub fn alloc(count: usize) -> (anyptr, error(MemoryError))
pub fn read(fd: i32, buf: []u8) -> (usize, error(IoError))
pub fn flush(fd: i32) -> error(IoError)
pub fn load(path: []u8) -> ([]u8, error(MemoryError | IoError))
```

`error(A | A)` (a duplicate member) is a sema error. Member order is
cosmetic — `error(A | B)` and `error(B | A)` are the same type.

Fallible return values must be captured, propagated with `try`, or
explicitly discarded with `_`; ignoring them is a sema error:

```mirage
alloc(n)                      // sema error: ignored error result
const ptr, _ := alloc(n)      // ok: explicitly discarded
const ptr := try alloc(n)     // ok: propagated
const ptr, err := alloc(n)    // ok: captured
```

### Internal Representation

The compiler generates a tagged union for every distinct `error(...)`
spelling: an outer `Ok`/`Failed` tag (`Ok` = 0, `Failed` = 1), where
`Failed` carries the error payload. For a single error type `error(T)`,
the payload is `T` directly — the representation is conceptually
`{tag: u32, payload: T}`. For a union `error(A | B | C)`, `Failed`'s
payload is itself a second tagged union dispatching on which member type
occurred. This generated type is never user-nameable and has no
user-accessible fields — all interaction goes through boolean coercion,
`return_ok`/`return_err`, `try`, and `match`/`switch`, described below.

### Boolean Coercion

An error value used in a boolean context (`if`, `while`, `&&`, `||`,
unary `!`, ternary condition) implicitly coerces to a check on the
`Ok`/`Failed` tag — `if err {}` is true when `err` is `Failed`; `if !err {}`
is true when `err` is `Ok`. This coercion applies only to error types and
only in boolean position.

### Error State Tracking

Sema tracks each error-typed local's state — `Unknown` (default),
`Failed`, or `Ok` — flow-sensitively through `if`/`while` branches,
block-scoped like any other local:

```mirage
const data, err := load_file(path)
if err {
    // err is known Failed here
} else {
    // err is known Ok here
}
```

**Early-return narrowing:** after `if !err { <body> }` where the condition
is exactly `!err`, there is no `else`, and every path through `<body>`
definitely exits (`return`/`return_ok`/`return_err`/`break`/`continue`),
`err` is narrowed to `Failed` in the code that follows:

```mirage
const data, err := load_file(path)
if !err { return_ok data }
// err is Failed here
match err { ... }   // legal
```

Reassigning a `mut` error-tracked variable, or taking its address with
`&`, resets its state to `Unknown`. Entering an `if err {}` (or
`if !err {}`) when the state is already known produces a warning for the
redundant check.

### `match` / `switch` on Error Values

Matching on an error value requires it to be known `Failed` at the match
site (narrow it first with `if err {}` or an early return); otherwise it's
a sema error. The match operates transparently on the inner payload — the
`Ok`/`Failed` wrapper is invisible to the user. For a single error type,
match goes directly to that type's own variants. For an error union, match
first dispatches on the member type, then (typically via a nested match)
on that member's own variants:

```mirage
if err {
    match err {
        .MemoryError(e): match e {
            .OutOfMemory: log("out of memory"),
            .NotFound:    log("not found"),
        },
        .IoError(e): match e {
            .NotFound(path):  fmt.printf("not found: {}\n", path),
            .Timeout(millis): fmt.printf("timed out after {}ms\n", millis),
            .Closed:          log("connection closed"),
        },
    }
}
```

Exhaustiveness follows the same rules as ordinary tagged-union/enum match
(§8): every member type and every variant of each member must be covered,
or a `_` default arm supplied.

### `return_ok` / `return_err`

See §6, "`return_err` and `return_ok`", for full syntax and sema rules.

### The `try` Expression

See §5, "`try` Expression". `try` requires the callee's `error(...)` type
be a subset of (or equal to) the caller's; on `Failed`, all deferred
statements in scope run before the enclosing function returns propagating
the error.

---

## 17. Variadic Arguments

There are two distinct kinds of variadic function, with different syntax and different rules.

### C-style Variadics (`ext fn`)

```mirage
ext fn printf(fmt: *u8, ...) -> i32
```

Only `ext fn` functions may take C-style `...` varargs, matching C ABI variadic-argument
promotion. In variadic calls, arguments after the fixed parameters must be at least 32 bits wide
(C default argument promotion rules). Valid variadic argument types: `i32`, `u32`, `i64`, `u64`,
`usize`, `f64`, typed pointers, `anyptr`. Narrower types (e.g., `f32`, `u8`, `i16`) must
be cast to a valid type before passing. `error(...)` values cannot be passed as C-style variadic
arguments — the generated type's shape varies per spelling and has no fixed C-ABI representation.
`expr...` spread ([Function Call](#function-call)) is not
valid for C-style varargs, since they carry no element type to check a slice against.

### Native Variadics (`fn f(args: ...T)`)

See [Native Variadic Parameters](#native-variadic-parameters). A `fn`'s final parameter may be
declared `...T`, dissolving to `[]T` inside the function body. Unlike C-style varargs:
- Trailing arguments are checked against the declared element type `T` like an ordinary parameter
  (including [implicit tagged-union coercion](#tagged-union-types) and literal defaulting) — no
  promotion-rule restrictions apply.
- Zero variadic arguments is legal (`nums` is an empty slice).
- An existing `[]T` (or `[N]T`) can be forwarded directly with `expr...` spread, without
  allocating a new array (see [Function Call](#function-call)).
- The function's address cannot be taken as a function pointer.
- Cannot appear in the same parameter list as a [default parameter value](#default-parameter-values)
  — a native variadic parameter has no default of its own, and the two features are not allowed to
  coexist in one parameter list at all.

---

## 18. Bitset Types

A `bitset` is a distinct named type representing a set of enum members
stored as bits in an integer. `bitset(...)` always declares a **new**
type — never an alias, even when two bitsets share the same member enum
and storage type.

### Declaration

```mirage
pub type Stream_Mode = enum(u8) {
    Close
    Flush
    Read
    Write
    Seek
    Size
}

pub type Stream_Modes = bitset(Stream_Mode)          // storage defaults to u32
pub type Stream_Modes16 = bitset(Stream_Mode, u16)   // explicit storage type
```

The first argument names the **member enum** and must resolve to an enum
type (`enum(u8) {...}`, `enum(i32) {...}`, or a plain `enum {...}` with no
parenthesized backing type — which, like any other enum declaration,
defaults to an `i32` backing type). Naming a non-enum type is a sema
error:

```
error: bitset member type must be an enum type
```

The optional second argument is the **storage type** and must be one of
`u8`, `u16`, `u32`, `u64`; any other type (a signed integer, a float, a
named type) is rejected. If omitted, the storage type defaults to `u32`.

### Bit-Index Range Check

Each member enum variant `v` occupies bit index `v.value + 1` (bit 0 is
never assigned) in the storage integer. This is validated at the `bitset`
declaration site — not at each use site — by requiring `bit_index <
storage_bits` for every variant:

```mirage
type Small = enum(u8) { A B C D E F G Query }   // Query = 7
type Bad = bitset(Small, u8)   // error: bit_index(Query) = 8, storage is only 8 bits
```

```
error: bitset variant 'Small.Query' has value 7, producing bit index 8
       (1 << 8 = 256), which does not fit in the storage type 'u8'
       (8 bits). Use a wider storage type or reduce the enum variant
       values.
```

### `sizeof` / `alignof`

`sizeof(BitsetType)` and `alignof(BitsetType)` equal the storage type's
size/alignment (e.g. 2 bytes for `bitset(E, u16)`).

### Literals

```mirage
const modes: Stream_Modes = {.Close, .Flush}   // set containing Close and Flush
const empty: Stream_Modes = {}                 // zero value — no bits set
const also_empty: Stream_Modes = default       // same as {}
```

A bitset literal is a braced set of `.Member` names (no `= expr`) — this is
what distinguishes it from a struct literal's `{.field = expr, ...}` form
(see §3, "Braced Initializers", and grammar.md). Each member is looked up
in the bitset's member enum; an unknown member name or a duplicate member
within the same literal is a sema error. A bitset literal (and `{}`/
`default` for a bitset-expected type) is only legal where an expected
bitset type is known from context.

### Operations

**Member set/clear/toggle** (target must be a bitset; RHS must be a single
`.Member` of the bitset's enum, or another value of the same bitset type):

| Operator | Meaning | Lowering |
|----------|---------|----------|
| `+=` | set member(s)    | `lhs \| rhs_bits`   |
| `-=` | clear member(s)  | `lhs & ~rhs_bits`  |
| `~=` | toggle member(s) | `lhs ^ rhs_bits`   |

Mixing a bitset with a raw integer on the right of any of these (e.g.
`modes += 5`) is a sema error; no other compound-assignment operator
(`*=`, `/=`, `&=`, `|=`, `^=`, `<<=`, `>>=`) is legal on a bitset target.
`~=` on a non-bitset target is likewise a sema error — use `^=` for
ordinary integer XOR-assign.

**Binary set operations** (both operands must be the same bitset type):

| Operator | Meaning | Lowering |
|----------|---------|----------|
| `+` | union               | `lhs \| rhs` |
| `\|` | union (synonym for `+`) | `lhs \| rhs` |
| `-` | difference          | `lhs & ~rhs` |
| `&` | intersection        | `lhs & rhs`  |
| `~` | symmetric difference | `lhs ^ rhs` |
| `^` | symmetric difference (same as infix `~`) | `lhs ^ rhs` |

`*`, `/`, `%`, `<<`, `>>`, and ordering comparisons (`<`, `>`, `<=`, `>=`)
are never legal between two bitsets (sets have no natural order or
magnitude).

**Unary `~`**: flips every bit of the storage integer, returning a value
of the same bitset type — `~a` where `a: Stream_Modes` is itself a
`Stream_Modes`.

**Equality**: `==`/`!=` compare the underlying storage integers directly
and are legal between two values of the same bitset type. Comparing a
bitset against a raw integer via `==`/`!=` (or any other operator) is a
sema error — `cast` to the storage type first.

**`in` — membership testing**:

```mirage
if .Close in modes { }               // single-member test
if {.Close, .Flush} in modes { }     // subset test
```

The left-hand side is a single `.Member` of the bitset's enum, or a
bitset value of the same type (including a braced literal); the
right-hand side is a bitset value. Semantics:
- single member `in` bitset: `(rhs & (1 << (member_value + 1))) != 0`
- bitset `in` bitset: `(rhs & lhs) == lhs` (subset check — this is the
  same formula the single-member case reduces to once the member is
  represented as its one-bit mask, so both shapes share one lowering)

When the left-hand side is a bare `.Member` or a braced literal, its
expected type is taken from the right-hand side's (already-resolved)
bitset type — `{.Close, .Flush} in modes` resolves the literal against
`modes`'s type, not the other way around. `in` on a non-bitset right-hand
side is a sema error ("`in` is only valid for bitset membership
testing").

### Coercion and Casting

**Implicit coercion — bitset → storage type, expected-type position
only**: a bitset value implicitly coerces to its storage type when (and
only when) the *expected* type at that position is exactly the storage
type — function arguments, a `const`/`mut` declaration with an explicit
type annotation, return values, assignment to a storage-typed lvalue:

```mirage
const modes: Stream_Modes = {.Close, .Flush}
const raw: u16 = modes            // OK — bitset -> its storage type, expected-type position
```

This coercion does **not** fire during binary-operator type resolution —
`modes + raw_u16` is a type mismatch, not an implicit-coercion
opportunity, even though `raw_u16`'s type is exactly the bitset's storage
type.

**No implicit coercion the other way** — a raw integer never implicitly
becomes a bitset:

```mirage
const raw: u16 = 3
const modes: Stream_Modes = raw            // error: type mismatch in assignment
const modes2 := cast(raw, Stream_Modes)    // OK — explicit cast
```

**`cast` rules**:
- `cast(bitset_value, storage_type)` — always legal, yields the raw integer.
- `cast(bitset_value, other_int_type)` — legal if the two integer types are
  cast-compatible per the ordinary integer cast rules; yields the integer.
- `cast(integer_value, BitsetType)` — always legal, no range check; the
  programmer is asserting the integer is a valid bitset value.
- `cast(bitset_a, BitsetTypeB)` where `A` and `B` are **different** bitset
  types — a sema error, **even if their storage types match**. Two
  distinct bitset types always require an explicit integer intermediary:
  ```mirage
  const b := cast(cast(a, u16), TypeB)   # OK
  const c := cast(a, TypeB)              # error: illegal cast between these types
  ```
  Casting a bitset to itself (`cast(a, TypeA)` where `a: TypeA`) is a
  legal no-op identity cast.

### `ext fn` Integration

Bitset types are legal in `ext fn` parameter and return position and are
**ABI-transparent**: at the C ABI boundary, a bitset value IS its storage
integer — exactly like an enum value is its underlying integer — so no
explicit coercion is needed at the call site in either direction:

```mirage
ext fn SDL_SetWindowFlags(window: *SDL_Window, flags: WindowFlags)
ext fn SDL_GetWindowFlags(window: *SDL_Window) -> WindowFlags

const flags: WindowFlags = {.Resizable, .Shown}
SDL_SetWindowFlags(win, flags)          // OK — flags passed as its storage integer

const current := SDL_GetWindowFlags(win)
if .Resizable in current { }            // immediately usable — no cast needed
```

### Worked Example

```mirage
pub type Stream_Mode = enum(u8) {
    Close
    Flush
    Read
    Write
    Seek
    Size
}

pub type Stream_Modes = bitset(Stream_Mode, u16)

ext fn apply_modes(flags: Stream_Modes)

pub fn main() -> i32 {
    mut modes: Stream_Modes = {.Close, .Flush}

    modes += .Write        // set
    modes -= .Close         // clear
    modes ~= .Flush         // toggle

    const required: Stream_Modes = {.Write, .Seek}
    const combined := modes + required     // union
    const common   := modes & required     // intersection

    if .Write in modes {
        modes += .Size
    }
    if {.Write, .Size} in modes {
        modes += .Seek
    }

    const raw: u16 = modes                 // implicit coercion to storage type
    const modes2 := cast(raw, Stream_Modes) // explicit cast from storage type

    apply_modes(modes)                     // ABI-transparent 'ext fn' call

    return cast(modes2, i32)
}
```

---

## 19. Reserved Keywords

The following identifiers are reserved by the language:

`asm` `bitset` `break` `byte` `cast` `const` `continue` `default` `defer` `else` `enum` `error` `ext` `false` `fn` `for` `if` `impl` `import` `import_bin` `in` `iota` `len` `macro` `match` `mut` `nil` `pub` `return` `return_err` `return_ok` `sizeof` `stackalloc` `struct` `switch` `trait` `true` `try` `type` `undefined` `union` `when` `while`

`ext` is parsed as an identifier, not a keyword; it is used as the prefix
for extern function declarations. `option`, `env`, `link`, and `warn` are
likewise parsed as plain identifiers, not keywords — they're only
meaningful immediately after the `#` sigil (`#option(...)`, `#env(...)`,
`#link(...)`, `#warn(...)`, see
[Compile-Time Configuration](#12-compile-time-configuration)) and remain
ordinary, unreserved identifiers everywhere else. `error` (used both as the
`error(T)` type syntax and after `#` in `#error(...)`) IS a reserved
keyword, unlike its three siblings above. `no_return`, `naked`,
`always_inline`, `section`, and `init` are likewise parsed as plain
identifiers, not keywords — meaningful only immediately after the `@`
sigil (`@no_return`, `@naked`, `@always_inline`, `@section(...)`,
`@init`, see [Declaration Attributes](#21-declaration-attributes)).

### Reserved for Future Use

The lexer also reserves the following identifiers, but nothing in the language currently uses them — writing any of them (e.g. as a variable or function name) is a parse error today, even though no feature consumes them yet:

- **`namespace`** — likely reserved for an explicit namespace-declaration feature; not implemented.
- **`offsetof`** — likely reserved as a future sibling to `sizeof` for computing a struct field's byte offset; not implemented.

`asm` is no longer in this list — see [Inline Assembly](#20-inline-assembly).

---

## 20. Inline Assembly

```mirage
pub fn open(filename: *u8, flags: i32, mode: i32) -> i32 {
    mut fd: i32 = undefined
    asm {
        mov rax, 2
        mov rdi, filename
        mov esi, flags
        mov edx, mode
        syscall
        mov &fd, eax
    }
    return fd
}
```

`asm { ... }` embeds raw x86-64 assembly (Intel syntax) directly in a
function body — for example, to issue a syscall with no `ext fn` wrapper
available. It is legal **only inside a function body**; at module scope it
is a sema error:

```
error: asm blocks are only legal inside function bodies
```

The lexer captures everything between the outer `{` `}` verbatim, tracking
brace depth for nesting, without applying any Mirage tokenization rules to
it — an asm block's body is lexed and parsed by an entirely separate,
internal grammar (see grammar.md's "Asm Statement" section for the full
EBNF).

### Instructions and Operands

One instruction per line: a mnemonic, followed by zero to three
comma-separated operands. `;` and `#` start a comment running to end of
line. An operand is one of:

- **A register** — one of the names in the table below.
- **An immediate** — a decimal or hexadecimal integer literal (`42`, `-5`,
  `0x2a`). Float immediates are not supported.
- **A Mirage variable, by value** — a bare identifier (`flags`) reads the
  variable's current value.
- **A Mirage variable, by address** — `&` immediately followed by an
  identifier, no whitespace (`&fd`) passes the variable's address, letting
  the instruction write through it. Only scalar and pointer-typed variables
  are valid asm operands (a struct/array/slice operand is a sema error);
  `const` and `mut` bindings are both valid — writing through `&const_var`
  is legal, consistent with the language's general rule that taking a
  binding's address and writing through it is always permitted.

An unknown variable name is a sema error:

```
error: unknown identifier 'fd' in asm block.
```

### Registers

| Width  | Registers |
|--------|-----------|
| 64-bit | `rax` `rbx` `rcx` `rdx` `rdi` `rsi` `rsp` `rbp` `r8`-`r15` |
| 32-bit | `eax` `ebx` `ecx` `edx` `edi` `esi` `esp` `ebp` `r8d`-`r15d` |
| 16-bit | `ax` `bx` `cx` `dx` `di` `si` `sp` `bp` `r8w`-`r15w` |
| 8-bit  | `al` `bl` `cl` `dl` `dil` `sil` `spl` `bpl` `r8b`-`r15b` |

Register names are case-insensitive. Each register belongs to a "family" —
its 64-bit root (e.g. `eax`'s family is `rax`) — used for clobber analysis:
writing to any width of a register clobbers the whole family.

### Operand Direction and Clobbers

Sema determines which operands an instruction reads versus writes using a
two-tier lookup by mnemonic:

**Tier 1** — an explicit table for common instructions:

| Mnemonic(s) | Operand directions |
|---|---|
| `mov`, `lea` | Write, Read |
| `movzx` | Write, Read (destination and source widths need **not** match — see below) |
| `add`, `sub`, `and`, `or`, `xor` | Read/Write, Read |
| `not`, `neg`, `inc`, `dec` | Read/Write |
| `cmp`, `test` | Read, Read |
| `push` | Read |
| `pop` | Write |
| `nop`, `ret`, `syscall` | (no operands) |
| `jmp`, `je`, `jne`, `jl`, `jle`, `jg`, `jge`, `ja`, `jae`, `jb`, `jbe`, `jz`, `jnz` | Read |
| `call` | Read |

**Tier 2** — any mnemonic not listed above falls through to a conservative
fallback: the first operand (if any) is assumed Read/Write, every
subsequent operand Read, with a warning:

```
warning: unknown mnemonic 'cpuid' — assuming first operand is read/write
         and remaining operands are read. Verify clobbers manually.
```

Sema is never blocked by an unrecognized mnemonic — it's passed through to
codegen as-is, which emits it verbatim into the assembled instruction (LLVM
may still accept or reject it).

**Implicit clobbers**, added regardless of the operand-direction table
above:

- `syscall` reads `rax`, `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9`; clobbers
  `rax` (return value), `rcx`, `r11` (destroyed by the kernel).
- `call` clobbers every System V caller-saved register: `rax`, `rcx`,
  `rdx`, `rsi`, `rdi`, `r8`, `r9`, `r10`, `r11`.
- `div`/`idiv` implicitly read `rdx:rax` and write `rax`
  (quotient)/`rdx` (remainder).
- `mul`/`imul` in their one-operand form implicitly read and write
  `rdx:rax`.

Every register clobbered by an instruction's Write/Read-Write operands,
plus each mnemonic's implicit clobbers, plus a memory clobber whenever any
`&var` operand appears anywhere in the block, are all threaded through to
the underlying LLVM inline-asm clobber list — this is what keeps the
surrounding function's own register allocation from colliding with the asm
block's effects.

### Width Checking

Where a register operand and a Mirage variable operand appear together in
one instruction, their widths must match:

| Mirage type | Required register width |
|---|---|
| `i8` / `u8` | 8-bit |
| `i16` / `u16` | 16-bit |
| `i32` / `u32` | 32-bit |
| `i64` / `u64` / `usize` / any pointer / `anyptr` | 64-bit |

A mismatch is a **warning**, not an error, naming a same-family register of
the correct width as the fix:

```
warning: asm operand 'fd' has type 'i32' (32 bits) but register 'rax' is
         64 bits. Consider using 'eax' instead.
```

For a `&var` (address-of) operand, the check applies to the *pointee*'s
width against the register used to write through it — the address itself
is always 64-bit and isn't what's being checked. Getting this wrong is not
just cosmetic: in Intel syntax without an explicit size specifier, LLVM
infers the memory write's size from the register's width, so `mov &fd,
rax` where `fd` is `i32` writes 8 bytes into a 4-byte slot, corrupting
whatever sits adjacent to it on the stack — exactly what the warning is
guarding against.

The same rule applies when a register name is written next to a by-value
variable operand of a different width (e.g. `mov rsi, flags` where `flags`
is `i32`): the loaded value is sized to the variable's own type, so it
must be paired with a register of that same width (`esi`, not `rsi`) for
the instruction to assemble at all.

**`movzx` is the one exception** to this whole section: zero-extending a
narrower source into a wider destination is its entire purpose, so its two
operands are *expected* to differ in width, and it is exempt from the
mismatch warning above. Instead sema enforces `movzx`'s real x86 encoding
constraints directly:

- The **source must be 8 or 16 bits**. There is no `movzx` encoding for a
  32-bit or 64-bit source — writing a 32-bit register already
  zero-extends its parent 64-bit register's upper half for free, so a
  plain `mov` (into the matching 32-bit sub-register) is what to use
  instead of reaching for `movzx` on a 32-bit value:
  ```
  error: 'movzx' source must be 8 or 16 bits (got 32 bits) — there is no
         'movzx' encoding for a 32-bit or 64-bit source; use 'mov' instead,
         which already zero-extends automatically when writing a 32-bit
         register
  ```
- The **destination must be strictly wider than the source**:
  ```
  error: 'movzx' destination width (32 bits) must be wider than source
         width (32 bits)
  ```

```mirage
mut wide: i32 = undefined
asm {
    movzx eax, byte_value   // byte_value: u8 -> zero-extended into eax (32-bit)
    mov &wide, eax
}
```

### Out of Scope for v1

The following are recognized (so they can be named precisely in a
diagnostic) but rejected as not supported in v1:

- SSE/AVX/FPU registers (`xmm0`-`xmm15`, `ymm0`-`ymm15`, `zmm0`-`zmm31`,
  `st0`-`st7`)
- Segment registers (`cs`, `ds`, `ss`, `es`, `fs`, `gs`)
- Control/debug registers (`cr0`-`cr4`, `cr8`, `dr0`-`dr3`, `dr6`, `dr7`)
- Labels and local jumps inside an asm block
- Memory operands with displacement/scale syntax (`[rsp + 8]`,
  `[rax*4 + rbx]`)
- Float immediate operands
- String operands
- Module-scope `asm` blocks (see above)

### Asm Expression

Everything above describes `asm { ... }` as a **statement** — useful for
side effects (`&var` writes) but unable to produce a value directly. A
second, **expression** form additionally names an output register whose
value at block-exit becomes the expression's value:

```mirage
asm -> eax { ... }              // result type inferred from context
asm -> eax: i32 { ... }         // result type explicit
```

Unlike the statement form, `asm -> reg [: type] { ... }` is a normal
expression: legal anywhere an expression is legal — `return`, a variable
declaration's initializer, a function-call argument, and so on — not just
as a bare statement. Its instruction body uses exactly the same asm-body
grammar (registers, immediates, Mirage variable operands, Tier-1/Tier-2
operand-direction analysis, clobbers, `movzx`'s exception, width checking)
as the statement form; only the header (`-> reg [: type]`) is new.

```mirage
pub fn open(filename: *u8, flags: i32, mode: i32) -> i32 {
    return asm -> eax {
        mov rax, 2
        mov rdi, filename
        mov esi, flags
        mov edx, mode
        syscall
    }
}
```

**Result type inference.** When `: type` is written explicitly, that is the
result type, full stop. Otherwise the result type is inferred from the
surrounding expected-type context — the same channel used for integer
literal defaulting, `.variant` resolution, and trait-handle coercion
elsewhere in the language: a `return` statement's declared return type, a
`var_decl_stmt`'s declared type, and so on. If neither an explicit `: type`
nor an expected-type context is available, it's a sema error:

```
error: cannot infer result type for 'asm -> eax'; add an explicit type
       annotation: 'asm -> eax: i32 { ... }' or annotate the variable
       receiving the result.
```

**Result register width checking.** The result register's width and the
resolved result type's width are compared using the same table as the
operand width check above. A mismatch is a **warning**, naming a
same-family register of the correct width as the fix:

```
warning: asm result register 'rax' is 64 bits but result type 'i32' is 32
         bits. Consider using 'eax' to avoid implicit truncation.
```

**Implicit clobber.** The result register is always implicitly read as the
block's output — its family is added to the clobber set even when no
instruction in the block explicitly writes it (e.g. a block that only
reads/computes into `eax` via instructions the Tier-1 table already
tracks needs no special handling; a block that never touches `eax` at all
still implicitly clobbers it, since the surrounding code cannot assume its
prior value survives).

**Composes with `&var` writes.** The register-return mechanism and `&var`
memory writes are completely independent and may be used together in the
same block:

```mirage
mut status: i32 = undefined
const result: i32 = asm -> eax {
    mov eax, 42
    mov &status, eax   // a second, independent output
}
```

**Unsupported/unknown registers** after `->` produce the same diagnostics
as an unsupported/unknown register operand inside the body (see "Registers"
and "Out of Scope for v1" above):

```
error: register 'xmm0' is not supported in inline asm (v1)
error: expected a register name after 'asm ->'
```

---

## 21. Declaration Attributes

`@name` / `@name(args)` precedes a `fn` declaration, attaching compiler-recognized metadata
to it. Attributes are currently legal only on `fn` (not `ext fn`, methods inside `impl`
blocks, or any other declaration kind), and `pub`, if present, comes after the attribute:

```mirage
@naked
pub fn raw_entry() {
    asm { ret }
}
```

### Syntax

```mirage
@naked                        // single — no arguments
@(naked, no_return)           // grouped — two or more attributes, no arguments
@section(".text.init")        // single — with an argument
```

Only one attribute *clause* may precede a declaration. Two separate clauses
(`@naked @no_return`) are a parse error; combine them with the grouped form
(`@(naked, no_return)`) instead. A grouped-form member never takes its own argument list —
`@(section(".text"))` is a parse error, since only the single `@name(args)` form takes
arguments. `@(naked)` (a single-member grouped form) is legal — there's no reason to
special-case it away.

### Known Attributes

`no_return`, `naked`, `always_inline`, `section`, and `init` are the only recognized names.
Any other name is a sema error:

```
error: unknown attribute '@foo'. Known attributes: no_return, naked, always_inline, section, init.
```

Every attribute argument (`@section`'s string, once arguments are involved) must be a
compile-time constant expression.

### `@no_return`

Declares that the function never returns control to its caller (it loops forever, calls a
process-exit primitive, etc.). The prologue/epilogue are unaffected — the only difference is
codegen marking the function (and every call to it) `noreturn` in the emitted LLVM IR, which
lets LLVM omit the (unreachable) successor block after a call.

A `@no_return` function whose return type is neither void nor `error(...)` is a warning, not
an error — the function still compiles, but the value return type can never actually be
returned:

```
warning: a '@no_return' function with a value return type will never return its value
```

### `@naked`

Suppresses the compiler-generated prologue and epilogue entirely: no stack frame setup, no
callee-saved register preservation, no implicit `ret`. The function body is expected to
handle all of that itself via inline `asm`.

If the body contains any statement other than an `asm { ... }` block, a warning is emitted
(not an error — the function still compiles, but a naked function's non-asm statements
implicitly assume a valid stack frame the compiler never set up):

```
warning: '@naked' function contains a non-'asm' statement; naked functions should contain
         only inline 'asm' blocks, since the compiler emits no prologue/epilogue for them
```

### `@always_inline`

Requests that the function be inlined at every direct call site (LLVM's `alwaysinline`
attribute). If the function's address is ever taken — assigned to a function-pointer-typed
variable, passed as a callback, returned, etc. — a warning is emitted, since calls made
through that pointer can't be inlined (the function still exists as an ordinary callable
symbol; only indirect calls lose the inlining):

```
warning: taking the address of '@always_inline' function 'f'. Calls through a function
         pointer cannot be inlined. The function will still exist as a symbol but the
         '@always_inline' attribute has no effect on indirect calls.
```

### `@section("name")`

Places the function in the named ELF section instead of the default `.text`. The argument
must be a non-empty compile-time-constant `[]u8` string; an empty string is a sema error.
The section name is passed to the linker verbatim — Mirage does not validate ELF section
naming conventions itself.

### `@init`

Marks the function as a module initializer, automatically called once before `main` (hosted
builds) or via the generated `_init` (freestanding builds — see below). Restrictions:

- Must take no parameters.
- Must return nothing or `error(SomeEnum)` — any other return type is a sema error.
- Is not allowed on a method inside an `impl` block (declare a module-scope function instead):

```
error: '@init' is not allowed on impl methods; declare a module-scope function instead
```

A module may declare more than one `@init` function; same-module `@init` functions always run
in source declaration order. Unlike ordinary functions, `@init` does **not** need to be `pub`
to be called from another module's ordering — the generated `_init` is emitted into the same
compilation as every other function in the program, so it can call a private `@init` function
directly. In fact, the common case is the opposite of `pub`: a module's initializer usually
has no business being an ordinary externally-callable symbol at all.

**Ordering.** If one module's `@init` function references a symbol (a variable read/write, or
a function call) declared in another module, that other module's `@init` function(s) — if any
— are guaranteed to run first. This dependency graph is built from actual symbol references
made in `@init` function bodies, not merely from the `import` graph: an `@init` function that
never touches a given imported module doesn't create an ordering dependency on it. A cycle in
this graph is a sema error naming both modules:

```
error: circular '@init' dependency: module 'A' requires module 'B' to initialize first, but
       'B' also (transitively) requires 'A' to initialize first
```

**Generated `_init`.** The compiler synthesizes a function named `_init` that calls every
`@init` function across the program in the order described above. In hosted builds, `_init`
is called from the generated `_start`, before `main`. If any `@init` function returns a
non-Ok `error(...)`, `_init` immediately terminates the process with exit code `1` — a fixed
sentinel, not the raw error value (which has no defined integer representation outside the
function's own context) — and `main` never runs.

In **freestanding builds** (`--freestanding`), `_init` is still generated, but `_start` is
not (as today, freestanding builds provide their own `_start`) — the user's `_start` is
responsible for calling `_init` itself at the appropriate time.

**`--noinit`.** Passing `--noinit` to the driver skips generating `_init` entirely (and, in
hosted builds, skips the `_start` call to it). Every `@init` function still compiles normally
and remains individually callable — only the automatic invocation is suppressed.

### Conflicting Attributes

Some combinations are rejected outright:

- `@naked` + `@always_inline` — a naked function has no compiler-generated body shape to
  inline.
- `@init` + `@naked` — init functions must be callable normally (from the generated `_init`).
- `@init` + `@no_return` — an initializer must return control so the next one can run.
- `@init` + `@always_inline` — init functions are called once, from generated code; inlining
  them defeats the purpose.
