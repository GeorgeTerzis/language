# language

a statically typed, systems language with a custom lexer, parser, and semantic analysis pass, written in C++23.
This is very much a work in progress. Things are half-finished and subject to change at any moment.

---

## Syntax

```
// Records and variants
record: @type @rec(
    a: s32;
    b: f32;
);

variant: @type @var(
    a: s32;
    b: f32;
);

// Functions
function: @fn(a: s32; b: s32) s32 = (
    @ret 10;
);

// Pipe and Block expresion example
function2: @fn(a: u32) u32 = a * 10 -> @pipe + 20 -> (
  @loop(i: u32 = 0) (i < @pipe) : (i += 1) (
    //...
  );
  @break @pipe * 30;
);

//expresion based function body
function3: @fn(a: s31; b: s31) = a + b;

// basic arena allocator
arena: @mod (
  t: @type @rec(
   data: *u8;
   offset: u64;
   capacity: u64;
  );

  next: @fn(arena: ~ @imut t) @ptr =
    arena::data + arena::offset;

  reset: @fn(arena: ~t) @void =
    arena::offset = 0;

  malloc: @::alloc::malloc_ft = (
    arena: ~t = @as(~t)ctx;

    @if(arena::offset + size > arena::capacity)
      (@ret @null);

    new: @ptr = next(arena);
    arena::offset += size;

    @ret new;
  );

  realloc: @::alloc::realloc_ft = (@unreachable);
  free: @::alloc::free_ft = (@unreachable);

  vtable: @const @::alloc::vtable = @(malloc; realloc; free);

  allocator: @fn(arena: ~t) @::alloc::t =
    @(@as(@ptr)arena; &vtable);
);

// Modules with generics
vector: @mod{T: @type}(
    t: @type @rec(
        begin:  *T;
        end:    *T;
        cursor: *T;
        allocator: alloc::t;
    );

    push: @fn(self: ~t; value: T) @var(success: ~T; fail: std::empty) = (
        // ...
    );
);

// Result types via variants
result: @mod{T: @type} (
    t: @type @var(
        value: T;
        empty: mono::t;
    );
);
```
the way I expect people to use it is the `@mod` and on the inside and it's functions `t: @type` pattern
so you can alias the `module: @mod{...}`  like so `bvec: vector{bool}` 

---

## Language features (so far)
- `@` - either is the start to a builtin keyword, compound literal or is used just like the `::namespace` in C++.
- `@rec` — struct/record types
- `@var` — tagged union / variant types
- `@fn` / `@fn*` — named and anonymous function types
- `@mod{T: @type}` — parameterized modules
- `@alias` — type and declaration aliases
- `@ptr` / `*T` / `~T` — opaque, mutable and immutable pointer types
- `@if` / `@ret` / `@break` — control flow
- `@as` — type casting
- `@pipe` / `->` — pipe operand and operator
- `@const` / `@imut`/ `@mut` — complietime and mutability qualifiers
- `@void` — void type
- Built-in numeric types: `s8`–`s128`, `u8`–`u128`, `f16`–`f128`, `bool`
- Integers can be of arbitary size so `u2`, `s124` are valid
---

## Architecture


**Lexer** (`frontend/lexer.hpp/.cpp`)
Table-driven dispatch lexer using `become` (musttail) for minimal dispatch overhead. Handles identifiers, numeric literals (int/float with separators and exponents), symbols, builtins (`@...`), symmetrical token pairs (parens, brackets, braces), and comments.

**Parser** (`frontend/parser.hpp`)
Produces a Grammar array that holds `median_t` (interior) and `final_t` (leaf/token) nodes. Uses the symmetrical index map from the lexer to jump over matched delimiters efficiently.

**Semantic analysis** (`frontend/semantics3.hpp/.cpp`)
Walks the Grammar array and builds a typed AST. Key structures:
- `decl_t` — declarations (bindings, functions, modules, aliases, type members)
- `type_t` — types (primitives, records, variants, function types, pointers, arrays, aliases)
- `expr_t` — expressions (literals, blocks, if, match, pipe, binary/unary ops, compound literals)
- `stmt_t` — statements (return, break, become, loops, imports, expression statements)
- `symbols_t` — scoped symbol table with parent chain lookup
- `env_t` — immutable environment passed through the analysis (symbols + AST parent + allocator)
- `pool_t` — arena-backed allocator that tracks all allocated nodes
- `deep_copy` — AST deep copy used for template instantiation

**Allocator** (`libs/llvm_allocator.hpp`)
Wraps LLVM's `BumpPtrAllocator` for fast, bulk-freed arena allocation.

---

## Dependencies

- **LLVM** — allocator, `APInt`/`APFloat`, `StringRef`, `StringSwitch`
- **Boost** — `mp11` (type-level metaprogramming), `pfr` (struct reflection), `program_options`
- **[frozen](https://github.com/serge-sans-paille/frozen)** — compile-time hash maps
- C++23 (`std::print`, `std::from_chars`, concepts)
- clang or any compiler that supports `[[clang::musttail]]`

Built with **Clang** (uses several Clang-specific attributes).

---

## Building

No build system is set up in the repo yet. This is how `compile.sh` does it:

```sh
#!/bin/bash
FLAGS='-std=c++26 -Wno-c++20-extensions -Wno-c++23-extensions -g0 -O3'

clang++ $FLAGS --shared -fPIC -o lexer.so  ./frontend/lexer.cpp  &
clang++ $FLAGS --shared -fPIC -o parser.so ./frontend/parser.cpp &
wait

echo "Compiling main"
clang++ $FLAGS $(llvm-config --libs) -o main ./main.cpp ./parser.so ./lexer.so -lboost_program_options
```

and from `run.sh`
```sh
#!/bin/bash
./main -f ../main.trt
```
---

## Status

| Stage | Status |
|---|---|
| Lexer | Working |
| Parser | Working |
| Semantic analysis | In progress |
| Type checking | Partial  |
| Template instantiation | Partial |
| LLVM IR lowering | Stubbed out / Partial |

Known rough spots noted in the source: `if` expression chains, symbol resolution for ambiguous identifiers, `@match`, and recursive type references through indirection.

---