# Mirage

Mirage is a compiled, statically-typed systems language that targets native code via LLVM. It is designed for low-level programming with a clean, expression-oriented syntax and direct interoperability with C libraries.

## Features

- **Static typing** with type inference (`const x := expr`)
- **Value semantics** — structs and arrays are copied on assignment
- **Pointer types** (`*T`) with explicit dereference and address-of
- **Slices** (`[]T`) — fat pointer/length pairs, no hidden allocation
- **Structs** with optional field defaults and packed layout
- **Enums** with `iota`, backed by any integer type
- **Untagged and tagged unions** (`union{}`, `union(enum)`)
- **`match`** expressions — exhaustive, with `_` wildcard
- **`defer`** — LIFO cleanup on block exit
- **`try`** — propagate non-zero `error` values up the call stack
- **`for val in slice`** — iterate slices by value or by reference (`&val`)
- **Function pointers** as first-class values
- **Multi-return functions**
- **Modules** via the filesystem; `import(...)` brings in namespaces
- **C interop** via `ext fn` declarations — no FFI layer needed

## Building

**Requirements:** CMake, Ninja, Clang, LLVM 15+

```sh
just build          # configure + build -> ./build/mirage
just install        # install to /usr/local/bin/mirage
```

Or manually:

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_STANDARD=23
cmake --build build
```

## Usage

```sh
mirage source.mir               # compile to ./a.out
mirage -o myprogram source.mir  # specify output path
mirage --emit-ir source.mir     # print LLVM IR instead of linking
mirage --freestanding source.mir
```

## Language Tour

### Hello World

```mirage
ext fn printf(fmt: *u8, ...) -> i32

pub fn main() -> i32 {
    printf("Hello, world!\n")
    return 0
}
```

### Variables

```mirage
const max: usize = 1024          // immutable
mut count: i32 = 0               // mutable, explicit type
mut name := "Mirage"             // mutable, inferred type
mut x: i32 = default             // zero-initialized
mut buf: [256]u8 = undefined     // uninitialized storage
```

### Types

```mirage
// Struct with field defaults
type Vec2 = struct {
    x: f32 = 0.0
    y: f32 = 0.0
}

// Enum backed by u8
type Direction = enum(u8) {
    north
    south
    east
    west
}

// Tagged union
type Shape = union(enum) {
    circle: struct { radius: f64 }
    rect:   struct { w: f64  h: f64 }
    point
}
```

### Functions and Methods

```mirage
fn add(a: i32, b: i32) -> i32 {
    return a + b
}

// Multi-return
fn divide(a: i32, b: i32) -> (i32, i32) {
    return a / b, a % b
}

impl Vec2 {
    pub fn length(self) -> f32 {
        return /* ... */
    }

    pub fn scale(mut self, factor: f32) {
        self.x *= factor
        self.y *= factor
    }
}
```

### Control Flow

```mirage
// if / else
if x > 0 {
    printf("positive\n")
} else {
    printf("non-positive\n")
}

// while
while i < len(items) {
    i++
}

// for-in over a slice
for val in items {
    printf("%d\n", val)
}

// with index
for i, val in items {
    printf("%d: %d\n", i, val)
}

// by reference (allows mutation)
for &val in items {
    *val *= 2
}

// match expression
const label := match direction {
    .north: "N",
    .south: "S",
    .east:  "E",
    .west:  "W",
}
```

### Error Handling

```mirage
fn read_file(path: *u8) -> ([]u8, error) {
    const fd := open(path, 0)
    if fd < 0 {
        return nil, 1
    }
    // ...
    return buf, 0
}

pub fn main() -> i32 {
    // try propagates non-zero errors to the caller
    const data, err := try read_file("input.txt")
    // ...
}
```

### defer

```mirage
fn process(path: *u8) -> error {
    const fd := open(path, 0)
    defer close(fd)       // runs when the block exits, in LIFO order

    // ...
    return 0
}
```

### C Interop

```mirage
ext fn open(path: *u8, flags: i32) -> i32
ext fn close(fd: i32) -> i32
ext fn write(fd: i32, buf: anyptr, n: usize) -> i64
```

Mirage strings are `*u8` (null-terminated), matching C conventions directly.

---

## Example: Hash Map

A chaining hash map built entirely from libc primitives — no runtime, no garbage collector.

```mirage
// Declare all the external libc functions needed
ext fn malloc(size: usize) -> anyptr
ext fn realloc(ptr: anyptr, size: usize) -> anyptr
ext fn free(ptr: anyptr)
ext fn memcpy(dest: anyptr, src: anyptr, n: usize) -> anyptr
ext fn memcmp(s1: *u8, s2: *u8, n: usize) -> i32

const TABLE_SIZE: usize = 101

pub type Node = struct {
    key: *u8
    key_len: usize
    value: anyptr
    next: *Node = nil
}

pub type Dictionary = struct {
    buckets: [TABLE_SIZE]*Node
    value_size: usize
}

fn compute_hash(str: []u8) -> usize {
    mut hash: usize = 5831
    for ch in str {
        hash = ((hash << 5) + hash) + cast(ch, usize)
    }
    return hash % TABLE_SIZE
}

fn duplicate(key: *u8, key_len: usize) -> *u8 {
    const data: *u8 = malloc(key_len)
    if data == nil {
        return nil
    }
    return memcpy(data, key, key_len)
}

impl Dictionary {
    pub fn init(mut self, value_size: usize) {
        self.clear()
        self.value_size = value_size
    }

    pub fn get_ptr(self, key: []u8) -> anyptr {
        const idx := compute_hash(key)
        const key_len := len(key)

        mut cur := self.buckets[idx]
        while cur != nil {
            if cur.key_len == key_len && memcmp(cur.key, key, key_len) == 0 {
                return cur.value
            }
            cur = cur.next
        }

        return nil
    }

    pub fn get(self, key: []u8, out: anyptr) -> bool {
        const ptr := self.get_ptr(key)
        if ptr == nil {
            return false
        }
        memcpy(out, ptr, self.value_size)
        return true
    }

    pub fn contains(self, key: []u8) -> bool {
        return self.get_ptr(key) != nil
    }

    pub fn put(mut self, key: []u8, value: anyptr) -> bool {
        const idx := compute_hash(key)

        const value_ptr := self.get_ptr(key)
        if value_ptr != nil {
            memcpy(value_ptr, value, self.value_size)
            return true
        }

        mut entry: *Node = malloc(sizeof(Node))
        if entry == nil {
            return false
        }

        entry.key_len = len(key)
        entry.key = duplicate(key, entry.key_len)
        if entry.key == nil {
            free(entry)
            return false
        }

        entry.value = malloc(self.value_size)
        if entry.value == nil {
            free(entry.key)
            free(entry)
            return false
        }

        memcpy(entry.value, value, self.value_size)

        entry.next = self.buckets[idx]
        self.buckets[idx] = entry

        return true
    }

    pub fn remove(mut self, key: []u8) -> bool {
        const idx := compute_hash(key)
        const key_len := len(key)

        mut cur := self.buckets[idx]
        mut prev: *Node = nil

        while cur != nil {
            if cur.key_len == key_len && memcmp(cur.key, key, key_len) == 0 {
                if prev != nil {
                    prev.next = cur.next
                } else {
                    self.buckets[idx] = cur.next
                }

                free(cur.key)
                free(cur.value)
                free(cur)

                return true
            }

            prev = cur
            cur = cur.next
        }

        return false
    }

    pub fn clear(mut self) {
        for &bucket in self.buckets {
            mut cur := *bucket
            while cur != nil {
                const next := cur.next

                free(cur.key)
                free(cur.value)
                free(cur)

                cur = next
            }
            *bucket = nil
        }
    }
}
```

## Documentation

Detailed language documentation lives in `docs/`:

- `docs/spec.md` — full language specification
- `docs/grammar.md` — formal grammar
- `docs/getting-started.html` — introduction and setup
- `docs/types.html` — type system reference
- `docs/functions.html` — functions, methods, and closures
- `docs/control-flow.html` — control flow constructs
- `docs/error-handling.html` — `error`, `try`, and `defer`
- `docs/structs-and-types.html` — composite types in depth
- `docs/modules.html` — module system
