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
| 12 | `as` (cast) |
| 13 | unary `-` `!` `~` |
| 14 | primary: literals, identifiers, calls, primitive calls, `( expr )` |

`as` is the one non-operator row: it takes a *type* on its right, not an expression, and chains left-associatively — `x as i8 as u64` is `(x as i8) as u64`. Because it sits between the binary operators and unary, `-x as i8` is `(-x) as i8` and `x + 1 as i32` is `x + (1 as i32)`.

## Typing rules

- `&&`, `||`, and `^^` (bool XOR) all take and produce `bool`, and **all three are eager — both operands are always evaluated**. They lower to the bitwise `AND`/`OR`/`XOR` on `bool`, which is the same operation for 0/1 (see `docs/middleend/JIR.md`). There is no short-circuiting in v1.
- `!` takes `bool`. `~` takes integers; `-` takes **signed** integers, because an unsigned type has no negative side to negate into — `~0u8` is `255`, and `-1u8` is a compile error. Bitwise `& \| ^ << >>` take integers.
- `==` / `!=` accept any two same-typed values, including two `bool`s. `< <= > >=` accept two same-typed **integers or chars** — a `bool` is neither, so `true < false` is a compile error, even though `bool` lowers to `i8` in the IR and the comparison would be well defined there. The rule is in the language, not inherited from the machine.
- Comparisons produce `bool`; a `bool` is not an integer, so `a < b < c`, `1 + true`, and an `i32` value as a condition are type errors. A condition (the grammar's `condition` in `if`/`while`/`do`/`for`, see `language.md#grammar`) must be `bool`.
- A call's argument count and types must exactly match the declaration (a *lone* literal argument takes the parameter type); the call's type is the function's return type. When a name has several declarations (an overload set — `statements.md#functions`), the arguments are typed **before any parameter is chosen**: each is parsed on its own, so a *lone* literal argument is an `i32` unless suffixed, and exactly one declaration must then match those types exactly — candidates are never converted to match. Zero matches is an error naming the argument types, e.g. `no overload of 'f' matches the arguments (i32, bool)`.
- **A `primitive` call is answered by the primitive catalog, not by a declaration** (`docs/primitive.md#calling`): its arguments must exactly match the primitive's signature (again, a lone literal argument takes the parameter type) and its type is the primitive's return type. `print` is the one variadic primitive — any number of value arguments, none of them `void`, result `void`. A primitive call is never a constant expression, and a plain `IDENT ( … )` call never resolves to a primitive.
- **For arithmetic (`+ - * / %`), bitwise (`| ^ &`), and shift (`<< >>`) operators: the expected type threads to the first operand when it is a lone operand (no following operator of the same precedence level). The second operand takes the first operand's type.** This makes `let x: i8 = 127 + 1` fold in `i8` (range error: 128 does not fit `i8`), and `let x: i64 = 1 + 2` fold in `i64` (result `3`).
- **For comparison (`== != < <= > >=`) and logical (`|| && ^^`) operators: the expected type does NOT thread.** The first operand defaults to `i32` when unsuffixed, the second takes the first's type, and the result is `bool`. This keeps `1 < 2` a comparison of two `i32` values producing `bool`.
- **`x as T` converts a value's type** (`types.md#casts`): any of the ten types to any other, wrapping into the target. The operand is parsed **without an expected type** — the target says everything — so a lone literal there is an `i32` unless suffixed. A `void` value has no bits to convert, so calling a `void` function and casting the result (`f() as i32`) is a compile error. Constant operands fold through the same wrap the machine applies.
- Constant expressions are folded at compile time, exactly, in the type of their operands; a fold that leaves that type, outgrows every type, or divides by zero, is a compile error. See `types.md#constants` for what a constant is and why a compile-time expression is checked where a runtime one wraps. (A cast is the stated exception: it wraps by definition — `types.md#casts`.)

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