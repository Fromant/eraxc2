# Expressions

The expression grammar lives in `language.md#grammar`; this file covers precedence and typing.

## Precedence

Lowest to highest (operators on the same line are left-associative):

| | Operators |
| - | --------- |
| 1 | `\|\|` |
| 2 | `^^` |
| 3 | `&&` |
| 4 | `\|` |
| 5 | `^` |
| 6 | `&` |
| 7 | `==` `!=` |
| 8 | `<` `<=` `>` `>=` |
| 9 | `<<` `>>` |
| 10 | `+` `-` |
| 11 | `*` `/` `%` |
| 12 | unary `-` `!` `~` |
| 13 | primary: literals, identifiers, calls, `( expr )` |

## Typing rules

- `&&`, `||`, and `^^` (bool XOR) all take and produce `bool`, and **all three are eager — both operands are always evaluated**. They lower to the bitwise `AND`/`OR`/`XOR` on `bool`, which is the same operation for 0/1 (see `docs/middleend/JIR.md`). There is no short-circuiting in v1.
- `!` takes `bool`. `-` and `~` take integers. Bitwise `& \| ^ << >>` take integers.
- `==` / `!=` accept any two same-typed values, including two `bool`s. `< <= > >=` accept two same-typed **integers or chars** — a `bool` is neither, so `true < false` is a compile error, even though `bool` lowers to `i8` in the IR and the comparison would be well defined there. The rule is in the language, not inherited from the machine.
- Comparisons produce `bool`; a `bool` is not an integer, so `a < b < c`, `1 + true`, and an `i32` value as a condition are type errors. A condition (the grammar's `condition` in `if`/`while`/`do`/`for`, see `language.md#grammar`) must be `bool`.
- A call's argument count and types must exactly match the declaration (literals adapt to the parameter type); the call's type is the function's return type.
- Constant expressions are folded at compile time; folding that overflows the expected type, or divides by zero, is a compile error. See `types.md#constants` for what a constant is and why a compile-time expression is checked where a runtime one wraps.

## Modulo follows the dividend, not the divisor

Division and modulus **truncate toward zero**, so the result takes the sign of the *left* operand — the sign of the divisor is ignored. This is what C99, Java, Rust, and Go do; it is *not* what Python, JavaScript, or Haskell do.

| Expression | Result | Why |
| ---------- | ------ | --- |
| `-7 / 2` | `-3` | toward zero, so `-3.5` becomes `-3` |
| `-7 % 2` | `-1` | `-7 = -3 * 2 + (-1)` |
| `7 / -2` | `-3` | the divisor's sign changes nothing |
| `7 % -2` | `1` | the result takes the dividend's sign |
| `-7 / -2` | `3` | signs cancel |

The rule is exact: `a == (a / b) * b + (a % b)` for any `b != 0`, and `(a / b) * b` never exceeds `a` in magnitude. Write the divisor's sign into the dividend (`if b < 0 { b = -b; a = -a; }`) if you need Python-style results.

An unsigned division is unsigned throughout, so there is no sign to carry and no such rule applies.