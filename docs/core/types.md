# Types and literals

The grammar for types is in `language.md`; these are the rules the parser enforces for types and literals.

## Types

| Name | Meaning |
| ---- | ------- |
| `i8`…`u64` | two's-complement integers, 8/16/32/64 bits |
| `bool` | exactly `true` / `false` |
| `char` | a byte: ASCII code point, range 0–255 |

Mapping to the IR: `bool` and `char` lower to `i8` (the type is preserved in the source; the machine just sees a byte).

## Literals

- Integer literals (decimal, `0x`/`0X`, `0b`/`0B`, `0o`/`0O`, `_` separators) are **contextual**: their type is the expected type of the position they fill — the declared type of `let`, the lvalue type of `=`, a parameter type in a call.
- The literal's value must fit the expected type: `let x: i8 = 300;` and `let y: u8 = -1;` are compile errors.
- Float literals (`3.14`, `1e10`, `1.5e-3`) lex as `Number` but have no type in v1; using one is a compile error (`float is not supported yet`).
- Character literals are `char`. String literals lex okay but have no type in v1: using one, or writing `string` where a type is expected, is a compile error (`string is not supported yet`). `string` itself is not a reserved word — see the keyword list in `language.md#reserved-words`.
- `true` / `false` are `bool` constants.

## No implicit conversions

Two operands of a binary operator must have the **exact same type**; an `=` / compound-assign RHS and a call argument must exactly match the target type — except that a literal (or a constant expression of literals) adapts to the expected type and is range-checked against it. Identifiers never silently widen or narrow. Mixing `i8` and `i32`, or `bool` and an integer, is a type error. (Widening and casts are future work.)