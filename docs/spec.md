# Mirage Language Specification

## Overview

Mirage is a compiled, statically-typed systems language that targets native code via LLVM IR. It is designed for low-level programming with a clean, expression-oriented syntax. Mirage compiles to native executables and can interoperate with C libraries via `ext fn` declarations.

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
| `error`  | Unsigned 64-bit integer; `0` means no error                    |
| `anyptr` | Untyped pointer; interoperates with all typed pointers and `nil` |

### The `anyptr` Type

`anyptr` supports arithmetic with integer operands (`+`, `-`) for pointer arithmetic. It can be assigned to and from any typed pointer or function pointer, and compared with `nil`.

---

## 2. Composite Types

### Pointer Types

```
*T
```

A typed pointer to a value of type `T`. Dereferenced with unary `*`. Address taken with unary `&`. Typed pointers are implicitly assignable to/from `anyptr`.

Auto-deref: accessing members or calling methods on a pointer-to-struct automatically dereferences the pointer.

### Array Types

```
[N]T
```

A fixed-size array of `N` elements of type `T`. `N` must be a compile-time constant expression. Arrays are value types (copied on assignment). Arrays are implicitly assignable to matching slice types.

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
# Qualified form
const c: Shape = Shape.circle{.radius = 3.0}

# Contextual form (type inferred from annotation)
mut r: Shape = .rect{.w = 4.0, .h = 5.0}

# Payload-free
const p: Shape = .point
```

Payload fields in constructors use the `.field = value` syntax inside braces.

Tagged union members cannot be accessed directly; use `match` to destructure.

### Function Pointer Types

```mirage
fn(ParamType1, ParamType2) -> ReturnType
fn(ParamType) -> (RetType1, RetType2)
fn(*u8, ...) -> i32
fn() -> void   # written as: fn()
```

Function pointer types represent callable values. They are opaque pointers internally (8 bytes). The `nil` literal is assignable to any function pointer type; `default` produces a null function pointer.

Multi-return function pointer types use `-> (T1, T2)` syntax.

---

## 3. Literals

### Integer Literals

```mirage
42          # decimal
0xFF        # hexadecimal
0b1010      # binary
1_000_000   # underscore separators allowed
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

Supported escape sequences: `\\`, `\"`, `\n`, `\t`, `\r`, `\0`.

String literals have type `*u8` (null-terminated). The null terminator is appended automatically.

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
mut name := expr          # type inferred from initializer
mut name: Type            # default-initialized (struct fields, arrays, etc.)
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
mut x: i32 = default      # zero
mut p: Point = default    # each field default-initialized
```

`default` initializes a value to its type-appropriate zero. For structs, each field is recursively default-initialized (using field-level default expressions if present).

### The `undefined` Initializer

```mirage
mut x: i32 = undefined    # storage allocated, no initialization
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
| 1     | `=` `+=` `-=` `*=` `/=` `&=` `\|=` `^=` `<<=` `>>=` | right |
| 2     | `?:` (ternary)                  | right         |
| 3     | `\|\|`                           | left          |
| 4     | `&&`                             | left          |
| 5     | `\|`                             | left          |
| 6     | `^`                              | left          |
| 7     | `&`                              | left          |
| 8     | `==` `!=`                        | left          |
| 9     | `<` `>` `<=` `>=`               | left          |
| 10    | `<<` `>>`                       | left          |
| 11    | `+` `-`                          | left          |
| 12    | `*` `/` `%`                      | left          |
| 13    | Unary: `-` `!` `~` `&` `*` `++` `--` `try`| right         |
| 14    | Postfix: call `()` `.member` `[idx]` `++` `--` | left |
| 15    | Primary                          |               |

### Arithmetic

```mirage
a + b    # add (also: anyptr + integer)
a - b    # subtract (also: anyptr - integer)
a * b
a / b
a % b
```

Both operands must have the same type (no implicit promotion). `anyptr` supports `+` and `-` with integer operands.

### Comparison

```mirage
a == b   # equal
a != b   # not equal
a < b
a > b
a <= b
a >= b
```

Result type is `bool`. Both operands must be of assignable types.

### Logical

```mirage
a && b   # logical AND (bool operands only)
a || b   # logical OR (bool operands only)
```

### Bitwise

```mirage
a & b    # bitwise AND
a | b    # bitwise OR
a ^ b    # bitwise XOR
a << b   # shift left
a >> b   # shift right
```

Both operands must have the same type.

### Unary

```mirage
-x       # numeric negation
!x       # logical NOT (bool only)
~x       # bitwise NOT
&x       # address-of (produces *T)
*x       # dereference pointer
```

### Ternary

```mirage
condition ? then_expr : else_expr
```

Both branches must have the same type.

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
x++    # post-increment
x--    # post-decrement
++x    # pre-increment  (prefix form in unary)
--x    # pre-decrement  (prefix form in unary)
```

Requires a mutable operand.

### Member Access

```mirage
value.field          # struct/union field
pointer.field        # auto-deref through pointer
module_name.symbol   # cross-module access
```

### Index and Slice

```mirage
arr[i]            # array or slice index
arr[start..end]   # slice expression (produces []T)
```

### Function Call

```mirage
fn_name(arg1, arg2)
obj.method(arg1)
fp(arg1)            # call through function pointer
mod.fn_name(arg)    # cross-module call
```

### `cast`

```mirage
cast(expr, TargetType)
cast(ptr, []T, length)   # create a slice from a pointer
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

Returns the size in bytes as `usize`. Note: built-in type keywords (e.g., `sizeof(u64)`) are not supported as operands; use a variable of that type.

### `len`

```mirage
len(array_or_slice)
```

Returns the number of elements as `usize`. Valid on array and slice types.

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

Calls a fallible function (one whose last return type is `error`). If the error value is non-zero, propagates the error by returning from the enclosing function; all deferred statements in scope run before the return. On success, evaluates to the non-error return values.

### Braced Initializers

```mirage
{.field = val, .field2 = val2} # struct initializer
{val1, val2, val3}             # array initializer
{}                             # empty (full default initialization)
```

An array initializer's last value may end with `...` to fill all remaining elements with that same value (evaluated once):

```mirage
mut npc_ids: [10]i32 = { -1... }              # all 10 elements set to -1
const levels: [10]i32 = { 0, 1, 2, 3, 4, 5... } # 0, 1, 2, 3, 4, then five more 5s
```

### Import Expression

```mirage
const mod := import("path/to/module")
```

`import(...)` is only valid as the initializer of a `const` declaration with no explicit type. It binds the imported module as a namespace.

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
    # then
} else if other_condition {
    # else if
} else {
    # else
}
```

The `else` branch is optional. The condition must be `bool`. The body can be any statement (not necessarily a block).

### While Loop

```mirage
while condition {
    # body
}
```

Loops while `condition` is `true`. The condition is `bool`. The body must be a block statement. `break` exits the loop; `continue` jumps to the next iteration.

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
return val1, val2    # multi-return
```

Returns from the current function. For multi-return functions, multiple values are returned separated by commas. Deferred statements run before the actual return.

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
mut x: i32           # default-initialized
const y := 10
mut a, b := func()   # group declaration
```

---

## 7. Functions

### Regular Functions

```mirage
fn name(param1: Type1, mut param2: Type2) -> ReturnType {
    # body
}

pub fn name(p: Type) -> (T1, T2) {
    # multi-return
}
```

- Parameters are immutable by default; `mut` makes a parameter mutable.
- `pub` makes the function visible to importing modules.
- Multi-return: `-> (T1, T2, ...)` syntax.
- Void return: omit the `->` clause.

### Extern Functions

```mirage
ext fn puts(s: *u8) -> i32
ext fn printf(fmt: *u8, ...) -> i32   # variadic
pub ext fn malloc(size: usize) -> anyptr
```

Declares an external C function. `ext fn` functions:
- Accept at most one `...` at the end (variadic)
- `...` requires at least one named parameter before it
- Return at most one type
- Cannot have parameters or return types that are structs, arrays, slices, or unions

### Entry Points

The root module must define one of:
```mirage
pub fn main()           # void; exits with code 0
pub fn main() -> i32    # exits with this code
pub fn main() -> error  # exits with this error value (truncated to i32)
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
pub macro align_up(n: usize) -> (n + (alignment - 1)) & ~(alignment - 1)
```

Macros are expression-level compile-time substitutions. They are called with the same syntax as functions. Parameters are typed. The body is an expression template.

---

## 8. Match and Switch

Both `match` (expression) and `switch` (statement) use the same arm pattern syntax.

### Arm Patterns

**Variant pattern** (for enum and tagged union operands):
```mirage
.field_name
.variant_name(capture)     # binds payload struct by value
.variant_name(&capture)    # binds payload struct by reference (*PayloadType)
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
        # access self.field
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

---

## 10. Modules

### Importing a Module

```mirage
const io := import("path/to/module")
```

`import(...)` is valid only as the initializer of a `const` declaration with no explicit type. The path is a string resolved by the compiler. The result is a namespace binding.

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

---

## 11. Type Declarations

```mirage
type Name = TypeExpression
pub type Name = TypeExpression
```

Creates a named type alias. The named type is structural: two declarations with the same structure resolve to the same underlying type.

---

## 12. Type Inference

- `mut x := expr` infers the type of `x` from `expr`.
- `const x := expr` infers the type from `expr`.
- Function parameter types, return types, and `const`/`mut` with an explicit type annotation always resolve exactly.
- `default`, `undefined`, `.field` enum literals, and braced initializers require an expected type (from annotation or context) to be set.
- When calling a function that takes a known type, argument expressions are type-checked against that expected type.

---

## 13. Type Compatibility and Assignability

The following types are mutually assignable without explicit cast:

| From            | To            | Notes                                |
|-----------------|---------------|--------------------------------------|
| `anyptr`        | `*T`          | unsafe, no check                     |
| `*T`            | `anyptr`      |                                      |
| `anyptr`        | fn ptr type   | nil-to-fn-ptr coercion               |
| fn ptr type     | `anyptr`      |                                      |
| `[N]T`          | `[]T`         | array decays to slice                |

Arithmetic, bitwise, and other binary operations require both operands to have the same type (except `anyptr ± integer`).

---

## 14. Error Handling

The `error` type is an unsigned 64-bit integer. By convention, `0` means "no error" and any non-zero value is an error code.

### Fallible Functions

A function is fallible when its last return type is `error`:
```mirage
fn divide(a: i32, b: i32) -> (i32, error) {
    if b == 0 { return 0, 1 }
    return a / b, 0
}
```

Fallible return values with an `error` component must be captured; ignoring them is a sema error.

### The `try` Expression

```mirage
mut result: i32 = try divide(10, 2)

# In a group declaration:
mut q, e := divide(20, 4)
```

When used as `try call(...)`, if the error return is non-zero, `try` propagates the error by returning it from the enclosing function. The enclosing function must itself return `error` as its last return type.

### `defer` and Error Propagation

Deferred statements registered at the time of `try` propagation run before the enclosing function returns with the error. This enables cleanup regardless of whether an error occurs.

---

## 15. Variadic Arguments

Only `ext fn` functions may be variadic. In variadic calls, arguments after the fixed parameters must be at least 32 bits wide (C default argument promotion rules). Valid variadic argument types: `i32`, `u32`, `i64`, `u64`, `usize`, `error`, `f64`, typed pointers, `anyptr`. Narrower types (e.g., `f32`, `u8`, `i16`) must be cast to a valid type before passing.

---

## 16. Reserved Keywords

The following identifiers are reserved by the language:

`break` `byte` `cast` `const` `continue` `default` `defer` `else` `enum` `error` `ext` `false` `fn` `for` `if` `impl` `import` `iota` `len` `macro` `match` `mut` `nil` `return` `sizeof` `struct` `switch` `true` `try` `type` `undefined` `union` `while`

Note: `for` is reserved but not yet implemented.

`ext` is parsed as an identifier, not a keyword; it is used as the prefix for extern function declarations.
