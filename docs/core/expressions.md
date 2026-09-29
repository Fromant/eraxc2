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

- `&&` / `||` take and produce `bool` and short-circuit on the left operand. `^^` (bool XOR) also takes and produces `bool`, but cannot short-circuit — both operands are always evaluated.
- `!` takes `bool`. `-` and `~` take integers. Bitwise `& \| ^ << >>` take integers.
- `==` / `!=` accept any two same-typed values; `< <= > >=` accept two same-typed integers or chars.
- Comparisons produce `bool`; a `bool` is not an integer, so `a < b < c`, `1 + true`, and an `i32` value as a condition are type errors. A condition (`if`/`while`/`for`'s cond) must be `bool`.
- A call's argument count and types must exactly match the declaration (literals adapt to the parameter type); the call's type is the function's return type.
- Constant expressions are folded at compile time; folding that overflows the expected type, or divides by zero, is a compile error.