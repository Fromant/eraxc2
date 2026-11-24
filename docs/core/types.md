# Types and literals

The grammar for types is in `language.md`; these are the rules the parser enforces for types and literals.

## Types

| Name | Meaning |
| ---- | ------- |
| `i8`…`u64` | two's-complement integers, 8/16/32/64 bits |
| `bool` | exactly `true` / `false` |
| `char` | a byte: ASCII code point, range 0–255 |
| `ptr T` | a pointer to a cell holding a `T` — the cell's address (`&x` / `*p` / `*p = v`, `expressions.md#typing-rules`) |
| `ref T` / `ref mut T` | safe reference to a target cell, with read or mutation access |
| named structure, including `Box<T>` | nominal owned value declared by `struct` |
| `ptr ptr T` | a pointer to a pointer: `ptr T` is a type like any other, so it nests with no special rule |

Mapping to the IR: `bool` lowers to `i8` and `char` to `u8` (a `0–255` byte, so character comparison and character `/ % << >>` are unsigned). The type is preserved in the source; the machine just sees a byte — and a pointer lowers to `ptr T`, a single machine word that holds the **address of a cell** (`JIR.md#types`). `ptr T` describes what a read through the pointer yields; the pointer cell itself is always one word, whatever `T` is.

## Literals

`null` denotes the zero raw pointer. Its type comes from an explicit pointer
context such as an annotation, assignment, a typed parameter, a return type, or
the left operand of `p == null` or `p != null`. It cannot infer a pointer type
on its own or initialize a safe reference.

- Integer literals (decimal, `0x`/`0X`, `0b`/`0B`, `0o`/`0O`, `_` separators) have an integer type.
- A literal may carry a **type suffix** naming that type: `5i64`, `0xFFu8`, `1_000i32`. The suffix set is the integer type names — `i8` `u8` `i16` `u16` `i32` `u32` `i64` `u64` — and nothing else; `char` and `bool` are not suffixes (`docs/frontend/tokenizer.md#numbers`).
- A **lone** literal with no suffix takes the type of the position it fills: the declared type of `let`, the lvalue type of `=`, a parameter type in a call, the return type of a function. Its value must fit that type, so `let x: i8 = 127;` is fine and `let x: i8 = 128;` is not. A call whose name is an overload set has no single parameter to hand a lone literal before resolution: such an argument is an `i32` unless suffixed, and the overload set must match it (`expressions.md#typing-rules`).
- A lone literal with no suffix and **no expected type** — an expression statement whose value is discarded — is an **`i32`**. So `1 + 2;` is an `i32` addition, which is constant and folds away; nothing reaches the IR.
- **For arithmetic (`+ - * / %`), bitwise (`| ^ &`), and shift (`<< >>`) operators, the expected type is threaded to the first operand when it is a lone operand** (no operator follows it). This makes `let x: i8 = 127 + 1` fold in `i8` and range-check (error: 128 does not fit `i8`), and `let x: i64 = 1 + 2` fold in `i64` (result `3`). The second operand always takes the first operand's type.
- **For comparison (`== != < <= > >=`) and logical (`|| && ^^`) operators, the expected type is NOT threaded.** Their result type (`bool`) differs from their operand type, so the first operand defaults to `i32` when unsuffixed. This keeps `1 < 2` a `bool` comparison of two `i32` values.
- A suffix wins over the position, and the two disagreeing is an error rather than a silent conversion: `let x: i32 = 5i64;` is a compile error. There are no implicit conversions of any kind.
- A literal is never a `bool`. `let b: bool = 1;` is a compile error, and so is `let c: i8 = 'A';` — the character does not convert to an integer. A lone integer literal *does* take a `char` position (`let c: char = 255;`), because `char` is a one-byte integer type; what it cannot do is name one, because `char` is not a suffix — outside a `char` position, `65 as char` is how an expression gets a `char` (`#casts`).
- `-` on a literal is read with the sign in view, so the negative half of a signed type is spelled that way: `-128i8` is `i8min` and `-200` does not fit an `i8`.
- `-` is **not defined on an unsigned type**, which has no negative side: `let x: u8 = -1;` is a compile error. `~` is the same on both signednesses and stays.
- Float literals (`3.14`, `1e10`, `1.5e-3`) lex as `Number` but have no type in v1; using one is a compile error (`float is not supported yet`).
- Character literals are `char`. Double-quoted literals have type `std.string` from the root standard library. Each evaluation produces an owned string and requires runtime allocation, so a string literal is not a constant expression. The bare name `string` is not an alias for `std.string`.
- `true` / `false` are `bool` constants.

## No implicit conversions

Two operands of a binary operator must have the **exact same type**; an `=` / compound-assign RHS and a call argument must exactly match the target type — except for a *lone* literal, which takes the type of its position and is range-checked against it. Identifiers never silently widen or narrow. Mixing `i8` and `i32`, or `bool` and an integer, is a type error. (The explicit scalar/raw-pointer conversion, `as`, is next.)

**For arithmetic, bitwise, and shift operators, the expected type threads to the first operand when it stands alone.** This means `let x: i8 = 127 + 1` is a range error (128 does not fit `i8`), not a type error. The first operand becomes `i8`, the second inherits `i8`, the addition folds to `128i8`, and the range check fails. Similarly, `let x: i64 = 1 + 2` is now valid: the addition folds to `3i64`.

**For comparison and logical operators, the expected type does NOT thread.** `1 < 2` compares two `i32` values and produces `bool`. `let b: bool = 1 < 2` is valid because the comparison result is `bool`, matching the position.

## Casts {#casts}

`x as T` is the **only** conversion in the language, and it is explicit: the scalar and raw-pointer types — `i8` … `u64`, `bool`, `char`, and each `ptr T` — cast to one another. Structures and safe references cannot be created, weakened, or reinterpreted with `as`; a same-type cast preserves an existing value. Values never change type on their own; if one needs a different type, the source says so.

The rule has one shape:

- The value is **read in the type it already has**, and its two's-complement pattern is **wrapped into the target type** — the low bits, reinterpreted at the target's width. This is the same wrap arithmetic does, so a cast is a bit operation, never a range check. A pointer is an unsigned word in this rule: `ptr T as u64` is the address's bits, and `u64 as ptr T` rebuilds a pointer from bits — which is also the only way to write the **null pointer**, `0 as ptr T` (any pointer whose bits are all zero).
- Casting **to `bool`** is the nonzero test: every value but `0` is `true`. Casting **from `bool`** yields its value, `0` or `1`.
- `char` takes part as its byte: `char` ↔ an integer behaves exactly like the same-width unsigned type (`u8`).

```
let a: i8 = 12888 as i8;        // 88   — 0x3258, low byte 0x58
let b: u8 = -1 as u8;           // 255  — the wrap of -1
let c: u64 = -1i32 as u64;      // 18446744073709551615
let d: bool = 42 as bool;       // true — nonzero
let e: i32 = true as i32;       // 1
let f: char = 65 as char;       // 'A'
let n: ptr i32 = 0 as ptr i32;  // the null pointer — no cell
```

**A constant cast wraps exactly like a runtime one.** `200 as i8` folds to `-56`, `(-1) as u8` folds to `255`, `0 as ptr i32` folds to the null pointer; there is no compile-error form of `as` for range. This is the deliberate carve-out from the rule under [`Constants`](#constants): arithmetic on constants is checked because the source named every operand and a result outgrowing the type is a mistake, while a cast *asks* for a new type and the answer is the wrap.

The operand of `as` is parsed **without an expected type** — the target already says everything — so a lone literal there is an `i32` unless it carries a suffix: in `1 as u8` the `1` is an `i32` that then wraps, and `1u8 as u8` is a no-op on an already-`u8` value. A cast whose target is the type it already has is legal and does nothing.

`as` binds tighter than every binary operator and looser than unary `-`/`!`/`~` (`expressions.md#precedence`): `x + 1 as i32` is `x + (1 as i32)`, `-x as i8` is `(-x) as i8`. To cast a sum, parenthesize it: `(x + 1) as i32`.

A value of type `void` has no bits to convert — `f() as i32` where `f` returns nothing is a compile error.

## Capabilities {#capabilities}

A **capability** is a named set of operations a type supports. Capabilities are a closed vocabulary in v1, and they exist to constrain a template's type parameters (`statements.md#templates`):

| Capability | Operators | Types that satisfy it |
| ---------- | --------- | --------------------- |
| `Ordered` | `<` `<=` `>` `>=` | the integer types and `char` |
| `Negatable` | unary `-` | the signed integer types |
| `Arithmetic` | `+` `-` `*` `/` `%` | the integer types and `char` |
| `Clonable` | value copying | scalars, raw pointers, references, and records with a valid copy implementation |
| `Movable` | explicit `move` | scalars, raw pointers, references, and records whose owned fields support movement |

- A type **structurally** satisfies a capability when the operator rules already accept those operators on it (`expressions.md#typing-rules`) — nothing is declared and there is no `implements`. `u8` satisfies `Ordered` and `Arithmetic` but not `Negatable`; `bool` satisfies no arithmetic, ordering, or negation capability (it is not ordered, has no arithmetic, and cannot be negated), even though `==`/`!=` accept any two same-typed values. A pointer (`ptr T`) satisfies `Clonable` and `Movable`, but none of the three operator capabilities: it has no order that means anything in the source (`==`/`!=` still accept two same-typed pointers), its cell offset takes a `u64` rather than a second pointer, and it cannot be negated — so a `ptr` never deduces a template parameter bound to `Ordered`/`Negatable`/`Arithmetic`.
- `char` is an integer byte, so it takes part in `Ordered` and `Arithmetic` as its unsigned value (`#types`); being unsigned, it is not `Negatable`.
- `Clonable` and `Movable` are independent capabilities; supporting one does not imply the other. The current scalar and pointer types support both without user declarations. Copying or moving a pointer copies its address, not its pointee.
- Capabilities are independent: `Negatable` does not imply `Arithmetic`, so a template lists every capability its body uses.
- A cast (`#casts`) works on every type and needs no capability.
- The bound is checked at each call, against the deduced type (`expressions.md#typing-rules`) — which is why `abs(12u8)` is rejected for wanting `Negatable` rather than failing somewhere inside the body.

## Structures

A structure is a nominal type: two declarations with equal fields remain different types. Generic structure applications such as `std.vector<i32>` are also types, with concrete fields and methods per type argument set. `ptr T`, `ref T`, and nested structure applications can contain type parameters. No implicit numeric, structure, or reference conversion is performed.

All fields, including nested record handles, occupy one machine cell. The reference lowering stores a record's ordered fields in an owned heap block and carries its handle as one value. Nested values retain value semantics; a copied record does not silently share owned fields. This storage representation is part of lowering, not a user-visible constructor or address encoding. Use portable pointer offset operations rather than arithmetic on address bits.

A structure without a user destructor receives structural copying when all fields are clonable. A user destructor disables automatic shallow copying; a `clone(): Structure` method supplies copying explicitly. Generic clone bodies are checked when copying is requested. `Clonable` is independent of `Movable`: a unique owner may move while copying is forbidden. Plain reference fields are copied as references. Recursive owned-field cycles are rejected; raw pointer and reference edges do not imply recursive ownership.

## References

`ref T` and `ref mut T` refer to an existing storage place. `ref value` borrows it for reading; `ref mut value` requires mutation permission. `*reference` reads its target and `*reference = value` requires mutable reference access. Fields and methods automatically follow a reference receiver. Taking a safe reference to an unnamed temporary is rejected. `const` fixes the reference binding; its `ref mut` type still permits modifying the target.

An ordinary instance method may declare `ref mut? T` as its return type. Its body
uses read-only `this` and returns an explicit conditional borrow such as
`return ref mut? this.field;`. The borrow must project through the receiver.
An explicit raw dereference with receiver provenance follows the manual raw
storage contract. At the call site the result is `ref mut T` through mutable
receiver access and `ref T` through read-only receiver access. A plain
`ref T` return type always grants read-only access.

Mutable aliases are permitted. A read-only reference restricts access through that reference; it does not freeze the object against other authorized access. Ordinary scalar mutation preserves references. Moving, destroying, replacing an owned value, or invalidating collection storage must not precede a later use of a reference to the affected target. Whole-vector references and references to vector elements have distinct dependencies. Method effects propagate storage invalidation through calls. Free-function invalidation summaries propagate through known calls and typed member operations; uncertain aliases and generic effects are summarized conservatively. Additional mutable record-reference arguments to methods and factories are conservative invalidation boundaries.

The frontend infers reference dependencies through local bindings, fields, copies, moves, calls, and returns, including CFG back edges. A returned reference depends on reference arguments; an owned returned structure retains the dependencies of contained references. Dependencies on several inputs are combined conservatively. Stores through mutable aliases preserve these dependencies, and references used by a destructor are checked at cleanup. Reference fields can therefore be used without explicit lifetime parameters. A reference to a local cannot escape through a return, field, or mutable external destination.

Safe references to global scalar and raw-pointer cells are valid for the entire run. Mutable access requires a global declared with `mut`. Explicit safe references to global structure owners and their fields are rejected, as are globals containing safe references. Global owners may be used through methods during a call, but those calls cannot expose safe references to global storage. Raw pointers remain available for explicitly manual memory contracts. There is no `unsafe` keyword: dereferencing/casting raw pointers and explicit borrowing through a raw dereference are the programmer's responsibility. An ordinary `as` cast cannot turn a raw pointer into `ref`.

## Constants {#constants}

A **constant expression** is one the parser can evaluate while reading it: literals, unary `-` / `!` / `~`, arithmetic, comparisons, and `as` casts. It appears wherever a value is required — a `let` initializer, a global's initializer, an assignment RHS, a call argument. Reading a variable, or calling a function or a primitive, is not constant (`expressions.md#typing-rules`). `&x` is not constant either — reading a variable, it names a cell the parser does not know — so a scalar constant initializer cannot take an address. A pointer *comparison* over constants is constant, as any comparison is: `0 as ptr i32 == 0 as ptr i32` folds to `true`.

A scalar global's initializer *must* be a constant expression; record globals use the initialization rules in `statements.md#globals`. A `let` initializer need not be, but whatever part of it is constant is still folded.

Reading an earlier scalar global in a constant initializer is constant by bare or by qualified name alike — `g` and `n.g` are the same kind of read (`expressions.md#names`), both folded where the initializer is, and both limited to globals declared earlier in the file.

### Why a constant is checked where a runtime value wraps

This is the one place where compile time and runtime differ on purpose, so it is worth stating the reasoning:

- A **constant expression is a value the source names.** `let x: u8 = 255u8 + 255u8;` — both `255u8`s are written down, so the sum `510` is known now, and no `u8` holds it. Rejecting it costs nothing and catches a real mistake.
- A **runtime expression is a rule about the machine.** `a + b` on two `u8` has to do *something* for every pair of values. Wrapping is the only total choice short of undefined behaviour, and undefined behaviour is ruled out (`language.md#semantics`).

So: a constant that leaves its type is a **compile error**; the same operation on two variables **wraps**. Neither is a special case of the other. The one deliberate exception is `as` (`#casts`): a cast wraps by definition, constant or not, because the source is *asking* for a new type rather than a value outgrowing its own.

### The two states of a folded constant

A constant is evaluated exactly, as a number, in the type of its operands. It comes out in one of two states:

| State | Example | Meaning |
| ----- | ------- | ------- |
| **A value of that type** | `1 + 2` → `3`, `1i64 << 40i64` → `2^40` | a value in the type it was folded in; the position has to be that type |
| **In no type** | `4294967296u64 * 4294967296u64` = `2^64` | `too large for any type` — a compile error, never a wrapped number |

The second state is the one that is easy to get wrong, because wrapping a value that is in no type produces a number that looks legitimate: `2^64` wraps to `0`, and `0` fits everything. A constant never wraps into an answer — that is the whole point of the split above. (A cast is the exception that proves it: `as` is the one spelling that *means* wrap, so it has no "in no type" state — see `#casts`.)

There is no third state and no placeholder. A literal's type is named by its suffix or by the position it fills, never held open, so `18446744073709551615u64` is a `u64` value and `18446744073709551615` is an `i32` literal that does not fit. The only ops that can leave the range are `+`, `-` and `*`, and the **negation** of `imin`; division by zero and `%` by zero are compile errors too. `as` is not among them: a cast always lands *in* its target type, by wrapping (`#casts`). `<<` is *not* one of them either: it is checked in the type it is folded in, and the shift count masks to that width first, so `1i64 << 66i64` is `1 << 2` = `4`, not an overflow.

### A comparison reads values, not patterns

A comparison is done in the type of its operands, and reads the two **as numbers**, which is the only reading that gets a negative value right: `-1i32 < 1i32` read as bits is `0xFFFF... < 1`, and that answers false. Signed and unsigned comparison agree whenever both values are non-negative, so the same reading is right for an unsigned operand, and a `u64` above `i64max` has no signed reading at all — it is compared as the unsigned value it is. So `1u64 << 63u64 > 0u64` is true and `1u64 << 63u64 < 0u64` is false, and the folded result of either comparison is `true` / `false`, a `bool`.

### Negation: the same operation, two outcomes

Negating a value is worth calling out because the two spellings differ, and both are correct:

```
let b: i32 = -(-2147483647i32 - 1i32);           // compile error: does not fit i32
let a: i32 = -2147483647i32 - 1i32;               // i32min, as a subtraction
let b: i32 = -a;                                  // fine: wraps to i32min
```

The first is a **constant** negation. The parser evaluates it, gets `2147483648`, and no `i32` holds that, so it rejects — even though the same value is reachable on line two, where the parser never has to *negate* it. The second `-a` is a **runtime** negation of a value it does not know, and the machine's rule is wrapping: `-i32min` is `i32min` in two's complement. See the reasoning above. This is the clearest single illustration of the compile-time/runtime split.

The `-` in front of a *literal* is not that second negation: it is read with the sign in view, because there is no other way to write the negative half of a range. `-128i8` is `i8min`, and it is a value.
