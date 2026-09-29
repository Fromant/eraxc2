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

- Integer literals (decimal, `0x`/`0X`, `0b`/`0B`, `0o`/`0O`, `_` separators) are **contextual**: their type is the expected type of the position they fill — the declared type of `let`, the lvalue type of `=`, a parameter type in a call.
- A position with **no expected type** — an expression statement whose value is discarded, as in `1 + 2;` — types its literals as `u32`.
- The literal's value must fit the expected type: `let x: i8 = 300;` and `let y: u8 = -1;` are compile errors.
- Float literals (`3.14`, `1e10`, `1.5e-3`) lex as `Number` but have no type in v1; using one is a compile error (`float is not supported yet`).
- Character literals are `char`. String literals lex okay but have no type in v1: using one, or writing `string` where a type is expected, is a compile error (`string is not supported yet`). `string` itself is not a reserved word — see the keyword list in `language.md#reserved-words`.
- `true` / `false` are `bool` constants.

## No implicit conversions

Two operands of a binary operator must have the **exact same type**; an `=` / compound-assign RHS and a call argument must exactly match the target type — except that a literal (or a constant expression of literals) adapts to the expected type and is range-checked against it. Identifiers never silently widen or narrow. Mixing `i8` and `i32`, or `bool` and an integer, is a type error. (Widening and casts are future work.)

## Constants {#constants}

A **constant expression** is one the parser can evaluate while reading it: literals, unary `-` / `!` / `~`, arithmetic, and comparisons. It appears wherever a value is required — a `let` initializer, a global's initializer, an assignment RHS, a call argument. Reading a variable, or calling a function, is not constant (`expressions.md#typing-rules`).

A global's initializer *must* be a constant expression. A `let` initializer need not be, but whatever part of it is constant is still folded.

### Why a constant is checked where a runtime value wraps

This is the one place where compile time and runtime differ on purpose, so it is worth stating the reasoning:

- A **constant expression is a value the source names.** `let x: u8 = 255 + 255;` — both `255`s are written down, so the sum `510` is known now, and no `u8` holds it. Rejecting it costs nothing and catches a real mistake.
- A **runtime expression is a rule about the machine.** `a + b` on two `u8` has to do *something* for every pair of values. Wrapping is the only total choice short of undefined behaviour, and undefined behaviour is ruled out (`language.md#semantics`).

So: a constant that leaves its type is a **compile error**; the same operation on two variables **wraps**. Neither is a special case of the other.

### The three states of a folded constant

A constant is evaluated in the `i64` range, because that is the widest range a source can write. A folded constant is in exactly one of three states:

| State | Example | Meaning |
| ----- | ------- | ------- |
| **A value** | `1 + 2` → `3`, `1 << 40` | fits `i64`; range-checked against the position's type |
| **A `u64` with no `i64` value** | `1 << 63`, `18446744073709551615` | valid as `u64` only; carried as a bit pattern, checked against the position |
| **In no type** | `4294967296 * 4294967296` = `2^64` | `too large for any type` — a compile error, never a wrapped number |

The third state is the one that is easy to get wrong, because wrapping a value that is in no type produces a number that looks legitimate: `2^64` wraps to `0`, and `0` fits everything. A constant never wraps into an answer — that is the whole point of the split above.

**A placeholder.** An untargeted literal is held at the full `u64` width rather than at the type it is first read as, and takes the type of the position it fills. This is what keeps `1 < 2` a `bool` (a comparison's own type wins) while `let x: i32 = 1 + 2;` is an `i32`. Only a *lone* literal is a placeholder — the expected type is not threaded into the operands of an operator, which is why `let x: i64 = 1 + 2;` is an `i64` by its position and `1 + 2` alone is a `u32`.

### Negation: the same operation, two outcomes

Negating a value is worth calling out because the two spellings differ, and both are correct:

```
let b: i32 = -(-2147483647 - 1);                 // compile error: does not fit i32
let a: i32 = -2147483647 - 1;
let b: i32 = -a;                                 // fine: wraps to i32min
```

The first is a **constant** negation. The parser evaluates it, gets `2147483648`, and no `i32` holds that, so it rejects. The second is a **runtime** negation of a value it does not know, and the machine's rule is wrapping — `-i32min` is `i32min` in two's complement. See the reasoning above. This is the clearest single illustration of the compile-time/runtime split.
