# Expressions

The expression grammar lives in `language.md#grammar`; this file covers precedence and typing.

## Precedence

Lowest to highest (binary operators on the same line are left-associative; unary prefixes nest right to left):

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
| 13 | unary `*` `&` `-` `!` `~` `move` `ref` / `ref mut` |
| 14 | primary: literals, qualified names, calls, primitive calls, `( expr )` |

`as` is the one non-operator row: it takes a *type* on its right, not an expression, and chains left-associatively — `x as i8 as u64` is `(x as i8) as u64`. Because it sits between the binary operators and unary, `-x as i8` is `(-x) as i8` and `x + 1 as i32` is `x + (1 as i32)`.

`*` and `&` appear at rows 11 and 6 as binary operators (**multiply**, **bitwise-and**); at row 13 the same `*` and `&` are prefix — **dereference** the operand's pointer and **address-of** the operand's cell. The position decides: a leading operator of row 13 is unary, anywhere a left operand of row 11/6 is expected it is binary. A deref binds tighter than every binary operator and than `as`: `*p + 1` is `(*p) + 1`, `*p as u64` is `(*p) as u64`. See the pointer rules just below.

## Typing rules

- `&&`, `||`, and `^^` (bool XOR) all take and produce `bool`, and **all three are eager — both operands are always evaluated**. They lower to the bitwise `AND`/`OR`/`XOR` on `bool`, which is the same operation for 0/1 (see `docs/middleend/JIR.md`). There is no short-circuiting in v1.
- `!` takes `bool`. `~` takes integers; `-` takes **signed** integers, because an unsigned type has no negative side to negate into — `~0u8` is `255`, and `-1u8` is a compile error. Bitwise `& \| ^ << >>` take integers.
- `==` / `!=` accept same-typed scalar, pointer, or reference values, including two `bool`s and two same-typed pointers (which compare by the cell they name — equal iff the same cell). `< <= > >=` accept two same-typed **integers or chars** — a `bool` is neither, so `true < false` is a compile error, even though `bool` lowers to `i8` in the IR and the comparison would be well defined there, and a pointer is neither, so `&x < &y` is rejected: a pointer names a cell but the language gives no order to the addresses' positions. The rule is in the language, not inherited from the machine.
- **`p + index` pointer offset.** A raw pointer `ptr T` plus a `u64` advances by that many storage cells and returns `ptr T`. A lone unsuffixed offset literal takes type `u64`. Both addresses must designate live cells in the same allocation, even for offset zero. Null, out-of-bounds, and one-past offsets trap. Pointer addition is evaluated at runtime. It does not apply to safe references.
- **`*p` dereference.** The operand must be a raw pointer or safe reference type; the result is its target type. A deref is a *read*: `*p` is a value of the pointee type, usable wherever a value of that type is (`let x: i32 = *p;`, `f(*p)`, `*p + 1`). Reading through a pointer that names no cell is a runtime trap, not a compile-time error (`language.md#semantics`).
- **`&x` address-of.** The operand must be a **read of a variable or global** — a local, a parameter, or a global — that the parser can see naming a cell; the result is `ptr` to its type (`&x` where `x: i32` is `ptr i32`). An accessible structure field also has an address. Other computed values have no address: `&42`, `&(a + b)`, `&*p` are compile errors, and so is `&` of a read through a deref. Taking the address of the *same* variable twice yields the same cell — `&x == &x` is `true`.
- For a **store, the pointee is threaded back**: in `*p = v;` and `*p += v` (the statement forms, `statements.md#statements`), the right side is parsed with an expected type — the pointee. This is the one place a *lone* literal takes the pointee type, exactly as it would for a plain `x = v`: `*p = 127;` with `p: ptr i8` folds the `127` in `i8`, and `*p += 2` is the read-modify-write on the pointee. The pointee of a compound store must be an integer (`+=`, `-=`, … are integer operators).
- Comparisons produce `bool`; a `bool` is not an integer, so `a < b < c`, `1 + true`, and an `i32` value as a condition are type errors. A condition (the grammar's `condition` in `if`/`while`/`do`/`for`, see `language.md#grammar`) must be `bool`.
- A call's argument count and types must exactly match the declaration (a *lone* literal argument takes the parameter type); the call's type is the function's return type. When a name has several declarations (an overload set — `statements.md#functions`), the arguments are typed **before any parameter is chosen**: each is parsed on its own, so a *lone* literal argument is an `i32` unless suffixed, and exactly one declaration must then match those types exactly — candidates are never converted to match. Zero matches is an error naming the argument types, e.g. `no overload of 'f' matches the arguments (i32, bool)`.
- **A concrete declaration beats a template.** When a name carries a template (`statements.md#templates`), an ordinary declaration that matches the argument types exactly is chosen first and no template is considered; the template is the fallback. So `print('x')` calls the ordinary `print(char)`, never `print<T>` with `T = char`.
- **A call to a template is solved by deduction.** With no concrete match, the arguments — typed standalone, exactly as for an overload set, so a *lone* literal is an `i32` unless suffixed — are unified against the parameter types, recursively through pointer, reference, and generic structure shapes: each type parameter takes exactly one concrete type, and two positions naming the same `T` must agree (there are no conversions between them), so `min(3, 2u64)` is an error (`cannot deduce 'T' of 'min': the arguments give i32 and u64`). Each bound is then checked against the deduced type, or the call is an error — `abs(12u8)` gives `u8 does not satisfy Negatable (required by 'T' of 'abs')`. The call's type is the return type with the type parameters replaced, and the argument types are exactly the substituted parameter types.
- **A `primitive` call is answered by the primitive catalog, not by a declaration** (`docs/primitive.md#calling`): its arguments must exactly match the primitive's signature (again, a lone literal argument takes the parameter type) and its type is the primitive's return type. `print` is the one variadic primitive — any number of value arguments, none of them `void`, result `void`. A primitive call is never a constant expression, and a plain `IDENT ( … )` call never resolves to a primitive.
- **For arithmetic (`+ - * / %`), bitwise (`| ^ &`), and shift (`<< >>`) operators: the expected type threads to the first operand when it is a lone operand (no following operator of the same precedence level). The second operand takes the first operand's type.** This makes `let x: i8 = 127 + 1` fold in `i8` (range error: 128 does not fit `i8`), and `let x: i64 = 1 + 2` fold in `i64` (result `3`).
- **For comparison (`== != < <= > >=`) and logical (`|| && ^^`) operators: the expected type does NOT thread.** The first operand defaults to `i32` when unsuffixed, the second takes the first's type, and the result is `bool`. This keeps `1 < 2` a comparison of two `i32` values producing `bool`.
- **`x as T` converts a value's type** (`types.md#casts`): any non-`void` type to any other, wrapping into the target. The operand is parsed **without an expected type** — the target says everything — so a lone literal there is an `i32` unless suffixed. A `void` value has no bits to convert, so calling a `void` function and casting the result (`f() as i32`) is a compile error. Constant operands fold through the same wrap the machine applies.
- Constant expressions are folded at compile time, exactly, in the type of their operands; a fold that leaves that type, outgrows every type, or divides by zero, is a compile error. See `types.md#constants` for what a constant is and why a compile-time expression is checked where a runtime one wraps. (A cast is the stated exception: it wraps by definition — `types.md#casts`.)

## Explicit move

`move` is a unary expression at the same precedence as `*`, `&`, `-`, `!`, and `~`. Its operand must name an initialized local or parameter, or explicitly dereference raw initialized storage (`move *pointer`). Safe partial field moves are rejected. Its result has the same type, is not addressable, and carries a value rather than a new binding. The source is consumed; the result can initialize a binding, be passed to a function, or be returned. `statements.md#bindings-and-moves` defines flow and reinitialization. It is not a constant expression.

## Typed raw allocation

`alloc<T>(count)` returns `ptr T` to raw storage for `count` elements. The count has type `u64`. Each element occupies one storage cell. The allocator zeroes the cells and reserves at least one cell even for count zero. No value of type `T` is constructed by allocation. An allocation exceeding the machine limit traps before any narrowing of the count.

`free(pointer)` releases the allocation by its base address when no ordinary declaration shadows the built-in. It accepts any `ptr T`. A null pointer is accepted. The caller must destroy any live owned values before freeing the storage. `primitive free(pointer)` accepts the same source types. Allocation and freeing cannot appear in a constant expression.

## Structure and reference expressions

`value.field` reads an accessible field and also denotes its storage place for assignment or borrowing. Fields and instance methods automatically follow reference receivers. `Type.new(...)` and receiverless methods use the named structure type; a generic type is explicit, as in `std.vector<i32>.new()`. A method call supplies its implicit `this` argument automatically, while ordinary function arguments still require explicit `ref` or `move`.

`ref place` and `ref mut place` create safe reference values (`types.md#references`). The target must already exist. In a `ref mut?` accessor, `ref mut? place` marks a receiver projection whose returned access depends on the call site. A read-only path cannot recover mutation permission by copying an embedded mutable reference. `*reference` reads its target; `*reference = expr` requires a mutable reference. Ordinary primitive calls cannot accept record owners or safe references. Explicit raw pointer operations retain their separate manual contract.

Operands and arguments evaluate left to right: an earlier value read is captured before a later call can modify its source. This applies to ordinary binary operators, function calls, primitive calls, and factory field expressions. The eager logical operator rule is unchanged.

## Names

If the leading identifier resolves to a local or global value, a following dot is member access on that value rather than namespace qualification. Otherwise the namespace/type lookup rules below apply.

An identifier names a local, a global, a function, a template, a structure, or a namespace, resolved by *where it is written* (`language.md#namespaces`):

- A **bare name** searches the current scope's locals first, then the current namespace's top-level names, then each enclosing namespace outward to the root. A global reads as a value; a function or template is only a call's callee.
- A **namespace-qualified name** (`a.b.c`) resolves its leading segments as a namespace path — the first segment searches outward from the current namespace, each later one is a *direct member* of the namespace before it — and names the last segment inside the final namespace. Globals (a value), functions/templates (a callee), structures (a type/factory prefix), and namespaces (a prefix) live there, and a qualified name never names a local. A dotted **assignment target** (`n.g = 5;`) and a dotted **call** (`std.print(x)`) are this same resolved name.
- A name that resolves to a function or template outside a call is an error (`'f' is a function and can only be called`); one that resolves to a namespace outside a qualified prefix is an error too (`'n' is a namespace, not a value`).
- **Overload sets, the concrete-beats-template choice, and template deduction are per (qualified) name.** A qualified call considers exactly the declarations in the namespace its name names; the surrounding namespaces are not searched past that name's own segments. `print(1)` and `std.print(1)` are different calls to different declarations.

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
