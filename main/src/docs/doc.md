# Internals

## podlist_t<T>

- std::vector replacement
- uses memcpy
- move only
- manual allocation and deallocation
- uses malloc, realloc, free with no ability for custom allocator
- a bit of a failure
- faster than std::vector on the lexer
- range support, and std::span
- a good alternative would the llvm::SmallVector or just use std::vector

## island

type specialised, stable, arena allocator

## archpelago

a list of islands, so it acts more like a vector

## variant

a std::variant wrapper with some QOL stuff

## Lexer

- Produces a token_buffer_t that holds a ref to the soruce text, tokens and source information for each token, as well as a symetrical token map

## Grammar

- Uses 1 big vector
- final_t is podlist_t::it<token type>, can't have children
- median_t is a element that has $length children right after it on the buffer
- grammar::cursor_helper_t{} is used on the Semantics part to help with parsing and ast creation

## Semantics

- Traditional AST with the main difference is that it uses std::variant/ tagged unions instead of inheritance
- each node has a ast_t and specialised value, both point to eachother
- memory is pool based, so no deallocations

# Aliases

## declaration `@alias`

Purely lexical aliasing

- `alias: @alias my_type;`
- `alias: @alias my_variable;`
- `alias: @alias my_module;`

## `@type` alias

Used to give names to types and create aliases to other named types

- `mem_arena: @type @rec(ptr: @ptr; offset: u64; capacity: u64);`
- `mem_arena: @type @::core::allocators::arena::t;`

# Builtin Types

| Type     | Description                       | Example |
| -------- | --------------------------------- | ------- |
| `u<Num>` | unsigned integer of Num bits      | `u12`   |
| `s<Num>` | signed integer of Num bits        | `s32`   |
| `f<Num>` | floating point number of Num bits | `f128`  |
| `b<Num>` | boolean of Num bits               | `b1`    |

## [Integer and Boolean types](https://llvm.org/docs/LangRef.html#integer-type)(`u`, `s`, `b`)

Their bitsize can be arbitary with 2^23 being the bit limit from LLVM

## [Floating Point types](https://llvm.org/docs/LangRef.html#t-floating)(`f`)

| Type   | LLVM Type              |
| ------ | ---------------------- |
| `f16`  | `half`(IEEE-754)       |
| `f32`  | `float`(IEEE-754)      |
| `f64`  | `double`(IEEE-754)     |
| `f128` | `fp128`(IEEE-754)      |
| None   | `bfloat`               |
| None   | `x86_fp80`(x87)        |
| None   | `pcc_fp128`(2 64-bits) |

# Mutability Modifiers

| Modifier | Meaning            | Description                                                                                               |
| -------- | ------------------ | --------------------------------------------------------------------------------------------------------- |
| `@mut`   | Mutable            | Value can change at runtime.                                                                              |
| `@imut`  | Immutable          | Value cannot change after initialization.                                                                 |
| `@const` | Constant           | Value is known at compile time and cannot change.                                                         |
| none     | Wildcard/Inherited | Will inherit a mutability form higher on the chain or will act as a wildcard (with `@const` as exception) |

## Mutability Inheritance

When inheriting mutability the strictest ones always win\
for example `var: @imut @tup(@mut s32; u64)` then when we evaluate a expresion it will be treated as if it was a `@imut`\
in the example the `s32` is mutable but the `var` is immutable thus it is treated as `@imut`\
and the `u64` just inherits the mutablity of the type while in the AST it keeps a `NONE` flag for it's mutability

### TODO

We still have to figure out what will happen if the user takes in something like a `old: ~ @mut type` but wants to do `~ @imut type`
does have have to make directly a `~ @imut type` or can just say `new: @imut = old` and it will handle that.Maybe do `new: @val @imut` and that will handle the generics as well,
if we pass soemthing like `type` or `~/* type` it will figure out what we want

# Pointers

| Symbol       | Meaning           | Example       |
| ------------ | ----------------- | ------------- |
| `~`          | Immutable pointer | `~ @mut s32`  |
| `*`          | Mutable pointer   | `* @imut s32` |
| `@ptr`       | Opaque pointer    | `@ptr`        |
| `[<length>]` | (Static?) Array   | `[32]s32`     |

Format: `~`/`*` <type>
`@ptr` is our `void*` from C\
The usage of mutability modifiers on pointers is prohibited with the exception of `@const`\

- For `@ptr` everything is permited\
- Using `@mut` on `*` is redundant and on `~` is invalid\
- For `@imut` on `~` is redundant and on `*` is invalid

- `@mut ~s32`
- `@imut *s32`
- `@imut ~s32`
- `@mut *s32`

in the future I will add a `?` modifier to mark pointers as nullable, for now they act like C pointers
`?* s32`

# Modules `@mod`

Namespaces from C++
"Expandable"

```
module: @mod(
  t: @type @tup(s32; s32; s32);
);

// later in the file ...

module: @mod(
  function: @fn(...) t = (
    ....
  );
);
```

## Template Modules `@mod{....}`

The biggest difference form normal modules is that they are unique
so you can't "expand "them like you can with normal modules

Since we do not do Templates on types or functions we do them on Modules I am more strict on this than with the archetypes
so templates might stay only on Modules still open to change though

## Archetypes `@arch`

a design detail that must be answer is: can I partialy define a member of the archetype
for example

```
archetype: @arch (
  function: @fn; // the signature can be whatever
  function: @fn @void; // it can have whatever for an arguemnt but it returns @void/nothing
  function: @fn(arg: type;...) ret_type; // the signature must be fully defined
)
```

For now the plan is to use them only on modules but I am open on allowing them on functions and type as well
I was also thinking to make it easier to expand uppon it to integrate it with the attributes feature

# Functions

## Lambdas / Anonymous Functions

in the future I would like to find a good way to support lambdas like C++
but for now I want to keep everything simple, our implementation of lambdas would be simpler than C++
since we do not have to differentiate between references and values, if you want something by value you just pass it
if you want to reference to that you pass a pointer which is again just a value

## Function Templates

Used to alias fully defined function signatures so the function has it's arguments already named and the user just needs to define a body
is useful when a lot of functions need the same signature without having to change each one individualy to modify the signature

## Function Types

Function pointers form C

# Complex Types

## Records `@rec(<name>: <type>;...)`

Tuples with named members

## Tuples `@tup(<type>...)`

Records with anonymous members

## Variants `@var(<name>: <type>;...)`

example type def: `@var(success: *s32; failure: core::empty)` init: `@(success := 10)` or `@(failure: @())`
since they are named we do not need to have unique types
Tagged Unions that keep track what member is active

# Attributes `[[...]]`

```
arena: @mod [[@::alloc::arch; allocator; user; no_realloc; no_free]](
  t: @type [[@pub; @default]] @rec(
    data: @ptr;
    offset: u64;
    capacity: u64;
  );

  malloc: [[@priv; @malloc; @cleanup(free)]] @fn(...) @ptr = ...;
  free: [[@priv; @free; @producer(malloc; realloc)]] @fn(...) @ptr = ...;
  realloc: [[@priv; @realloc]] @fn(...) @void = ...;

  vtable: [[@pub]] @const @::alloc::vtable = @(malloc; free; realloc);
);
```

For now the feature is not even close to implementation
but the core idea is that the user can attach arbitary or builtin tags
to give the symbol/type/expresion/stmt some metadata to constrain,set requirements or categorise
and then query for those tags

- `user` indicator that this is not part of the compiler/standard lib
- `my_lib` indicator that this is part of a lib so we can query for every function
- `@arch`
- `@pub` & `@priv` for visibility in `@mod`
- `@default` or maybe `@default_export` for `@mod` example: `vector{s32}::t` with this it could be `vector{s32}` and it resolves it self. This would allow us to not need to have templates on types and functions since the default symbol can be used

are great candidates for that system

# `@as` operator

`@as(<type>)<expr>` when the expresion is a bool, float or integer literal then this is converted to a `fold_t` operand when doing expresion resolution

# C type mappings

This table is for `X86-64`(x64 doesn't always mean what you think it means)
This table lists the breakdown of sizes in the various programming models.

| Datatype    | LP64 | ILP64 | LLP64 | ILP32 | LP32 |
| ----------- | ---- | ----- | ----- | ----- | ---- |
| `char`      | 8    | 8     | 8     | 8     | 8    |
| `short`     | 16   | 16    | 16    | 16    | 16   |
| `_int`      | 32   | --    | 32    | --    | --   |
| `int`       | 32   | 64    | 32    | 32    | 16   |
| `long`      | 64   | 64    | 32    | 32    | 32   |
| `long long` | --   | --    | 64    | --    | --   |
| `pointer`   | 64   | 64    | 64    | 32    | 32   |

| Platform Family      | Typical Model |
| -------------------- | ------------- |
| Windows (all 64-bit) | **LLP64**     |
| Linux (x86-64/ARM64) | **LP64**      |
| macOS (64-bit)       | **LP64**      |
| BSDs (64-bit)        | **LP64**      |
| Solaris x86-64       | **LP64**      |
| AIX (Power)          | **LP64**      |

[ref](https://wiki.osdev.org/X86-64)

# Misc

- [LLVM IR Reference](https://llvm.org/docs/LangRef.html)
- [LLVM IR Programmer's manual](https://llvm.org/docs/ProgrammersManual.html)
- [Carbon Lang](https://github.com/carbon-language/carbon-lang)
- [Zig](https://ziglang.org/)
- [Ocaml](https://ocaml.org/)
- [OSdev](https://wiki.osdev.org/Expanded_Main_Page)
