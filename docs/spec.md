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
| `anyptr` | Untyped pointer; interoperates with all typed pointers and `nil` |

Note: `error` is not a scalar type — `error(T)` / `error(A \| B \| C)` is a
type-former used only in function return-type position. See §15, "Error
Type System".

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
# Qualified form
const c: Shape = Shape.circle{.radius = 3.0}

# Contextual form (type inferred from annotation)
mut r: Shape = .rect{.w = 4.0, .h = 5.0}

# Payload-free
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

const e: IoError = .NotFound("missing.txt")   # sugar for .NotFound{.v = "missing.txt"}
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
take(n)          # implicitly wrapped as Arg.size{.value = n}
take("hello")    # implicitly wrapped as Arg.str{.value = "hello"}
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

**Spread argument:** `expr...` forwards an existing slice as the variadic argument of a call to a
function with a native `...T` parameter (see [Variadic Arguments](#16-variadic-arguments)):

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

Calls a fallible function (one whose last return type is `error(...)`, see
§15). If the call's error result is in the `Failed` state, propagates it by
returning from the enclosing function; all deferred statements in scope run
before the return. On success, evaluates to the non-error return values, and
the callee's error result is discarded (there is nothing left to check — it
is `Ok`).

`try` requires the callee's `error(...)` type to be a subset of (or equal
to) the enclosing function's own `error(...)` type — every error member type
the callee can produce must also be a member of the caller's declared union.

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

### For Loop

```mirage
for x in 0..10 { ... }        # range, exclusive upper bound
for x in ..10 { ... }         # range with implicit lower bound of 0
for x in some_slice { ... }   # element by value
for i, x in some_slice { ... }  # index + element by value
for &x in some_slice { ... }    # element by reference (*T)
for i, &x in some_slice { ... } # index + element by reference
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
return val1, val2    # multi-return
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
is `error(...)`, see §15). Both run deferred statements before returning,
exactly like `return`.

`return_err <expr>` returns from the current function with the error result
in the `Failed` state, carrying `<expr>` as the payload; all non-error
return slots are `undefined` (matching what `try` emits on its
error-propagation path — the two lower identically). `<expr>` must name a
variant of one of the function's declared `error(...)` member types:

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

`return_ok [expr {, expr}]` returns from the current function with the
error result in the `Ok` state. The operands supply the non-error return
values in order, matching the function's non-error return types exactly via
the same checking rules as `return`.

```mirage
return_ok named_type          # -> (*ast.NamedType, error(E))
return_ok a, b                # -> (T1, T2, error(E))
return_ok                     # -> error(E)  (bare error-only return)
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

### Native Variadic Parameters

```mirage
fn sum(base: i32, nums: ...i32) -> i32 {
    mut total := base
    for n in nums {
        total += n
    }
    return total
}

sum(10)          # zero variadic args -> nums is an empty slice
sum(10, 1, 2, 3) # nums is []i32{1, 2, 3}
```

The final parameter of a `fn` may be declared `name: ...T`, where `T` is any valid element type.
Inside the function body, `name` behaves as an ordinary `[]i32` — a value of `[N]T` collected from
the trailing call arguments, or the slice passed directly via [spread](#function-call). A call must
supply zero or more trailing arguments assignable to `T` beyond the fixed parameters. This is
distinct from `ext fn`'s untyped C `...` varargs (see [Variadic Arguments](#16-variadic-arguments))
— the address of a variadic function cannot be taken as a function pointer.

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
pub fn main()               # void; exits with code 0
pub fn main() -> i32        # exits with this code
pub fn main() -> error(E)   # exits 0 on Ok, 1 on Failed
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

**Using a trait name in type position denotes a HANDLE, not the trait
definition.** A handle is a fat pointer — a data pointer plus a vtable
pointer — always 16 bytes, 8-byte aligned, regardless of which trait it
names.

### Implementing a Trait

```mirage
type Circle = struct { x: i32, y: i32, r: i32 }

impl Drawable for Circle {
    fn draw(self) { # draw the circle }
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
same `self`/`mut self`, same parameter types, same return types). A trait
impl may not contain methods beyond the trait's own surface — put those in a
separate bare `impl TYPE { }` block instead.

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
circle.draw()          # static dispatch: calls Circle's implementation directly
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
mut shapes: [2]Drawable = { &circle, &rect }   # &circle, &rect coerce to Drawable handles

for shape in shapes {
    shape.draw()        # dynamic dispatch: resolved through the handle's vtable at runtime
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

## 12. Type Declarations

```mirage
type Name = TypeExpression
pub type Name = TypeExpression
```

Creates a named type alias. The named type is structural: two declarations with the same structure resolve to the same underlying type.

---

## 13. Type Inference

- `mut x := expr` infers the type of `x` from `expr`.
- `const x := expr` infers the type from `expr`.
- Function parameter types, return types, and `const`/`mut` with an explicit type annotation always resolve exactly.
- `default`, `undefined`, `.field` enum literals, and braced initializers require an expected type (from annotation or context) to be set.
- When calling a function that takes a known type, argument expressions are type-checked against that expected type.

---

## 14. Type Compatibility and Assignability

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

## 15. Error Type System

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
    NotFound:  []u8   # carries the path that wasn't found
    Timeout:   u32     # carries the timeout value that elapsed
    Closed             # no payload
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
alloc(n)                      # sema error: ignored error result
const ptr, _ := alloc(n)      # ok: explicitly discarded
const ptr := try alloc(n)     # ok: propagated
const ptr, err := alloc(n)    # ok: captured
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
    # err is known Failed here
} else {
    # err is known Ok here
}
```

**Early-return narrowing:** after `if !err { <body> }` where the condition
is exactly `!err`, there is no `else`, and every path through `<body>`
definitely exits (`return`/`return_ok`/`return_err`/`break`/`continue`),
`err` is narrowed to `Failed` in the code that follows:

```mirage
const data, err := load_file(path)
if !err { return_ok data }
# err is Failed here
match err { ... }   # legal
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

## 16. Variadic Arguments

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

---

## 17. Reserved Keywords

The following identifiers are reserved by the language:

`break` `byte` `cast` `const` `continue` `default` `defer` `else` `enum` `error` `ext` `false` `fn` `for` `if` `impl` `import` `iota` `len` `macro` `match` `mut` `nil` `return` `return_err` `return_ok` `sizeof` `struct` `switch` `trait` `true` `try` `type` `undefined` `union` `while`

`ext` is parsed as an identifier, not a keyword; it is used as the prefix for extern function declarations.
