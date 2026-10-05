# Types and literals

The grammar for types is in `language.md`; these are the rules the parser enforces for types and literals.

## Types

| Name | Meaning |
| ---- | ------- |
| `i8`…`u64` | two's-complement integers, 8/16/32/64 bits |
| `bool` | exactly `true` / `false` |
| `char` | a byte: ASCII code point, range 0–255 |

Mapping to the IR: `bool` lowers to `i8` and `char` to `u8` (a `0–255` byte, so character comparison and character `/ % << >>` are unsigned). The type is preserved in the source; the machine just sees a byte.

## Literals

- Integer literals (decimal, `0x`/`0X`, `0b`/`0B`, `0o`/`0O`, `_` separators) have an integer type.
- A literal may carry a **type suffix** naming that type: `5i64`, `0xFFu8`, `1_000i32`. The suffix set is the integer type names — `i8` `u8` `i16` `u16` `i32` `u32` `i64` `u64` — and nothing else; `char` and `bool` are not suffixes (`docs/frontend/tokenizer.md#numbers`).
- A **lone** literal with no suffix takes the type of the position it fills: the declared type of `let`, the lvalue type of `=`, a parameter type in a call. Its value must fit that type, so `let x: i8 = 127;` is fine and `let x: i8 = 128;` is not.
- A lone literal with no suffix and **no expected type** — the first operand of an operator, or an expression statement whose value is discarded — is an **`i32`**. So `1 + 2;` is an `i32` addition, which is constant and folds away; nothing reaches the IR.
- A suffix wins over the position, and the two disagreeing is an error rather than a silent conversion: `let x: i32 = 5i64;` is a compile error. There are no implicit conversions of any kind.
- A literal is never a `bool`. `let b: bool = 1;` is a compile error, and so is `let c: i8 = 'A';` — the character does not convert to an integer. A lone integer literal *does* take a `char` position (`let c: char = 255;`), because `char` is a one-byte integer type; what it cannot do is name one, because `char` is not a suffix.
- `-` on a literal is read with the sign in view, so the negative half of a signed type is spelled that way: `-128i8` is `i8min` and `-200` does not fit an `i8`.
- `-` is **not defined on an unsigned type**, which has no negative side: `let x: u8 = -1;` is a compile error. `~` is the same on both signednesses and stays.
- Float literals (`3.14`, `1e10`, `1.5e-3`) lex as `Number` but have no type in v1; using one is a compile error (`float is not supported yet`).
- Character literals are `char`. String literals lex okay but have no type in v1: using one, or writing `string` where a type is expected, is a compile error (`string is not supported yet`). `string` itself is not a reserved word — see the keyword list in `language.md#reserved-words`.
- `true` / `false` are `bool` constants.

## No implicit conversions

Two operands of a binary operator must have the **exact same type**; an `=` / compound-assign RHS and a call argument must exactly match the target type — except for a *lone* literal, which takes the type of its position and is range-checked against it. Identifiers never silently widen or narrow. Mixing `i8` and `i32`, or `bool` and an integer, is a type error. (Widening and casts are future work.)

The expected type is **not threaded into an operator's operands**, which is what keeps `1 < 2` a `bool` and `let x: i32 = 1 + 2;` an `i32`. It also means an operator is in the type of its own operands, so `let x: i64 = 1 + 2;` is a type error and `let x: i8 = 100 + 27;` is one too — both operands are `i32`. Write the suffix to say which width is meant.

## Constants {#constants}

A **constant expression** is one the parser can evaluate while reading it: literals, unary `-` / `!` / `~`, arithmetic, and comparisons. It appears wherever a value is required — a `let` initializer, a global's initializer, an assignment RHS, a call argument. Reading a variable, or calling a function, is not constant (`expressions.md#typing-rules`).

A global's initializer *must* be a constant expression. A `let` initializer need not be, but whatever part of it is constant is still folded.

### Why a constant is checked where a runtime value wraps

This is the one place where compile time and runtime differ on purpose, so it is worth stating the reasoning:

- A **constant expression is a value the source names.** `let x: u8 = 255u8 + 255u8;` — both `255u8`s are written down, so the sum `510` is known now, and no `u8` holds it. Rejecting it costs nothing and catches a real mistake.
- A **runtime expression is a rule about the machine.** `a + b` on two `u8` has to do *something* for every pair of values. Wrapping is the only total choice short of undefined behaviour, and undefined behaviour is ruled out (`language.md#semantics`).

So: a constant that leaves its type is a **compile error**; the same operation on two variables **wraps**. Neither is a special case of the other.

### The two states of a folded constant

A constant is evaluated exactly, as a number, in the type of its operands. It comes out in one of two states:

| State | Example | Meaning |
| ----- | ------- | ------- |
| **A value of that type** | `1 + 2` → `3`, `1i64 << 40i64` → `2^40` | a value in the type it was folded in; the position has to be that type |
| **In no type** | `4294967296u64 * 4294967296u64` = `2^64` | `too large for any type` — a compile error, never a wrapped number |

The second state is the one that is easy to get wrong, because wrapping a value that is in no type produces a number that looks legitimate: `2^64` wraps to `0`, and `0` fits everything. A constant never wraps into an answer — that is the whole point of the split above.

There is no third state and no placeholder. A literal's type is named by its suffix or by the position it fills, never held open, so `18446744073709551615u64` is a `u64` value and `18446744073709551615` is an `i32` literal that does not fit. The only ops that can leave the range are `+`, `-` and `*`, and the **negation** of `imin`; division by zero and `%` by zero are compile errors too. `<<` is *not* one of them: it is checked in the type it is folded in, and the shift count masks to that width first, so `1i64 << 66i64` is `1 << 2` = `4`, not an overflow.

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
