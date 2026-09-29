# Core language — erax

Source-level contract of the erax language: grammar, types, and semantics.

- Lexing (tokens, trivia, literals' lexical shape) is specified in `docs/frontend/tokenizer.md`.
- Lowering and runtime behavior of the machine level are in `docs/middleend/JIR.md`; the source contract here is the part the parser must enforce and can reject.
- Roadmap and versioning live in `TODO.md`. This document describes **v1**.

## Style

- **Post-name type syntax.** Types are written after the name, everywhere: `let t: i32 = 123;`, params `fn foo(x: i32, y: bool)`, return `fn main(): i32 { … }`.
- **Explicit types only.** Every `let` and every param names its type; nothing is inferred.
- **Assignment is a statement, not an expression.** `a = b = c` is illegal (an assignment's value is never usable); compound forms like `a += 2` are the idiomatic mutation.
- **Required `;`.** Every terminating statement ends in `;`. No automatic semicolon insertion.
- A source file holds only **global declarations and function declarations** (top level). Everything else lives inside functions.

## Grammar

Terminals in quotes; `[]` optional, `{}` repeated, `|` choice. `INT`/`CHAR` are integer/character literals as lexed (see the tokenizer spec).

```
program        = { global | function } ;

global         = "let" IDENT ":" type "=" const-expr ";" ;
function       = "fn" IDENT "(" [ param { "," param } ] ")" [ ":" type ] block ;

param          = IDENT ":" type ;
type           = "i8" | "u8" | "i16" | "u16" | "i32" | "u32" | "i64" | "u64"
               | "bool" | "char" ;

block          = "{" { block | statement } "}" ;        // a block is itself a statement

statement      = let-stmt | assign | call-stmt | return-stmt
               | if-stmt | while-stmt | do-stmt | for-stmt
               | "break" ";" | "continue" ";" ;

let-stmt       = "let" IDENT ":" type [ "=" expr ] ";" ;
assign         = lvalue assign-op expr ";" ;
assign-op      = "=" | "+=" | "-=" | "*=" | "/=" | "%="
               | "&=" | "|=" | "^=" | "<<=" | ">>=" ;
call-stmt      = call ";" ;
return-stmt    = "return" [ expr ] ";" ;

if-stmt        = "if" "(" expr ")" block [ "else" ( if-stmt | block ) ] ;
while-stmt     = "while" "(" expr ")" block ;
do-stmt        = "do" block "while" "(" expr ")" ";" ;
for-stmt       = "for" "(" [ for-init ] ";" [ for-cond ] ";" [ for-step ] ")" block ;
for-init       = "let" IDENT ":" type "=" expr | lvalue assign-op expr ;
for-cond       = expr ;
for-step       = lvalue assign-op expr | call ;

expr           = or ;
or             = and { "||" and } ;
and            = bitor { "&&" bitor } ;
bitor          = bitxor { "|" bitxor } ;
bitxor         = bitand { "^" bitand } ;
bitand         = equality { "&" equality } ;
equality       = relational { ( "==" | "!=" ) relational } ;
relational     = shift { ( "<" | "<=" | ">" | ">=" ) shift } ;
shift          = additive { ( "<<" | ">>" ) additive } ;
additive       = multiplicative { ( "+" | "-" ) multiplicative } ;
multiplicative = unary { ( "*" | "/" | "%" ) unary } ;
unary          = ( "-" | "!" | "~" ) unary | postfix ;
postfix        = primary | call ;
call           = dotted-name "(" [ expr { "," expr } ] ")" ;
dotted-name    = IDENT { "." IDENT } ;
primary        = INT | CHAR | "true" | "false" | IDENT | "(" expr ")" ;

lvalue         = IDENT ;
const-expr     = expr limited to literals, the unary `-`/`!`/`~`, arithmetic and
                 comparison operators, and references to globals declared earlier
                 in the file;
```

Notes:

- Every non-void `primary` `IDENT` is a **read** of a variable or global. An `IDENT` naming a parameter is also a read.
- `dotted-name` followed by `(` is a call of a (possibly namespaced) function. A dotted name **not** followed by `(` is a syntax error — there is no member access in v1.
- `[` `]` lex but are not part of v1 (arrays are future work); using them is a syntax error.
- `const-expr` is enforced additionally: it must be constant-foldable and contain no calls.

## Operator set

A tokenizer `Operator` token is legal only if its full text is one of:

```
+ - * / %         arithmetic
<< >>             shifts
< <= > >=         relational
== !=             equality
& | ^             bitwise
&& ||             logical
= += -= *= /= %= &= |= ^= <<= >>=    assignment (statement-level)
! ~               unary (single-character)
```

Any other operator text — `===`, `++`, `--`, `**`, `=>`, `??`, `&&&`, … — is a compile-time `unknown operator` error. (The tokenizer's maximal munch guarantees ambiguity-free lexing: `a+++++b` is one token run and is rejected as an unknown operator.)

## Reserved words

`fn`, `let`, `if`, `else`, `while`, `do`, `for`, `break`, `continue`, `return`, `true`, `false`, and the ten type names `i8 u8 i16 u16 i32 u32 i64 u64 bool char`. They cannot be used as `IDENT` (declaration or reference). Type names are reserved because they carry their meaning in type position.

`string`, `std`, `print`, `text`, … are ordinary identifiers, not reserved words: they may be used as names. Writing `string` in *type* position is a forward-looking error (see [Literals](#literals)). `main` has the same status — the entry point is a rule on that name, see [Functions](#functions).

## Types

| Name    | Meaning                                  |
| ------- | ---------------------------------------- |
| `i8`…`u64` | two's-complement integers, 8/16/32/64 bits |
| `bool`  | exactly `true` / `false`                 |
| `char`  | a byte: ASCII code point, range 0–255    |

Mapping to the IR: `bool` and `char` lower to `i8` (the type is preserved in the source; the machine just sees a byte).

### Literals

- Integer literals (decimal, `0x`/`0X`, `0b`/`0B`, `0o`/`0O`, `_` separators) are **contextual**: their type is the expected type of the position they fill — the declared type of `let`, the lvalue type of `=`, a parameter type in a call.
- The literal's value must fit the expected type: `let x: i8 = 300;` and `let y: u8 = -1;` are compile errors.
- Float literals (`3.14`, `1e10`, `1.5e-3`) lex as `Number` but have no type in v1; using one is a compile error (`float is not supported yet`).
- Character literals are `char`. String literals lex okay but have no type in v1: using one, or writing `string` where a type is expected, is a compile error (`string is not supported yet`). Writing `string` as a *name* is fine — it is not reserved.
- `true` / `false` are `bool` constants.

### No implicit conversions

Two operands of a binary operator must have the **exact same type**; an `=` / compound-assign RHS and a call argument must exactly match the target type — except that a literal (or a constant expression of literals) adapts to the expected type and is range-checked against it. Identifiers never silently widen or narrow. Mixing `i8` and `i32`, or `bool` and an integer, is a type error. (Widening and casts are future work.)

## Statements

- **`let`.** Declares a variable in the current scope; the name must not already exist in that scope (shadowing an outer scope is allowed). Without an initializer, the variable must receive a **guaranteed assignment before any read** (definite assignment — a compile-time check over all paths; see [Semantics](#semantics)). An initializer read of the variable itself (`let x: i32 = x;`) is an error.
- **Assignment.** `lvalue op expr`; `lvalue` is a variable or global declared in an enclosing (or the same) scope. `a += 1` reads `a`, writes it, and is equivalent to `a = a + 1` (so `a` must already be definite-assigned). Only the types below apply per operator.
- **Expression statements.** Must be a call: `foo();`, `std.print(x);`. A lone `x;` or `5;` is a compile error (no side effects).
- **`return`.** In a non-void function, `return expr` on every path (reaching-return check) with `expr` of exactly the return type. In a void function, `return;` or falling off the end. `return expr` in a void function is an error.
- **`if` / `else`.** Condition must be `bool`. `else` binds to the nearest `if`; `else if` is spelling for a nested `if`.
- **`while` / `do` / `for`.** Conditions are `bool`. `for` is C-shaped: `for (init; cond; step)` where *init* is a `let` (scoped to the loop) or an assignment, *cond* is `bool`, *step* is an assignment or a call. All three clauses are optional (a bare `for(;;)` is an infinite loop).
- **`break` / `continue`.** Legal only inside a loop body (dynamically nested in `while`/`do`/`for`); `break` in a non-loop is an error. In the example, `break` inside an `if` inside a `for` is fine.

## Expressions and precedence

Lowest to highest (operators on the same line are left-associative):

| | Operators |
| - | --------- |
| 1 | `\|\|` |
| 2 | `&&` |
| 3 | `\|` |
| 4 | `^` |
| 5 | `&` |
| 6 | `==` `!=` |
| 7 | `<` `<=` `>` `>=` |
| 8 | `<<` `>>` |
| 9 | `+` `-` |
| 10 | `*` `/` `%` |
| 11 | unary `-` `!` `~` |
| 12 | primary: literals, identifiers, calls, `( expr )` |

- `&&` / `||` take and produce `bool`, short-circuit on the left operand.
- `!` takes `bool`. `-` and `~` take integers. Bitwise `& \| ^ << >>` take integers.
- `==` / `!=` accept any two same-typed values; `< <= > >=` accept two same-typed integers or chars.
- Comparisons produce `bool`; a `bool` is not an integer, so `a < b < c` and `1 + true` are type errors.
- A call's argument count and types must exactly match the declaration (literals adapt to the parameter type); the call's type is the function's return type.
- Constant expressions are folded at compile time; folding that overflows the expected type, or divides by zero, is a compile error.

## Semantics

- **By-value, no references.** Variables hold values; there are no pointers, aliasing, references, or lvalue-producing operators.
- **Scoping.** A `block` (and a `for`'s init) opens a scope; enclosing scopes are visible, inner names shadow outer ones. A name is in scope from its declaration to the end of its block. Globals are visible everywhere, including inside functions and other global initializers — but a global initializer may reference only globals **declared earlier** in the file.
- **Definite assignment.** Every read of a variable is preceded, on every execution path, by a write (`let` initializer or an assignment). Parameters and globals always have a value. This is enforced statically; it never produces runtime behavior.
- **Runtime arithmetic.** All integer arithmetic wraps (two's complement), exactly as the IR defines. Division/modulo truncate toward zero; `x / 0` (and `imin / -1`) terminate the process — no UB. Shift counts are masked to the operand width (`count & (width-1)`), so no shift is undefined.
- **No undefined behavior.** Anything the rules don't define — reading an undefined variable, wrong type, mismatched arity, unknown operator — is a compile-time error, not runtime behavior.
- **Determinism.** One evaluation order (left-to-right for the given associativity/precedence), single-threaded, no implicit state; the same program behaves identically on every backend.

## Functions

- Parameters are `IDENT : type`, comma-separated; the return type is `[ ":" type ]` — absent means `void`.
- A function body is a single `block`. Recursion is ordinary: every call gets its own frame (see `docs/middleend/JIR.md`).
- Arguments are passed **by value** and evaluate left to right; the callee reads copies in its own frame.
- A function sees only its own parameters/locals and the globals — never another function's locals.
- **Entry point.** The program must declare exactly one `fn main(): i32` (no parameters). Its return value is the process exit code, and it runs after all global initializers have run. `main` is a rule on the *name* (it is not a reserved word); no other function may be named `main`.

## Globals

A `global` is a top-level `let` with a required, explicitly written initializer:

```
let counter: i64 = 0;
let multiplier: i32 = 2 + 3;
```

There is **no implicit zero**: every global must be given its starting value in the source. Backends still zero the storage before running init code (that is an implementation detail of JIR); the language contract is that a global's value is the one its initializer wrote, in declaration order, before `main` runs. Globals are writable at runtime (`counter = counter + 1;`).

## Machine-level contract

The language lowers 1:1 to JIR:

| Source | JIR |
| ------ | --- |
| `bool`, `char` | `i8` |
| `i8`…`u64` | same |
| local / param | a vreg at a frame offset |
| global | a `@` global, zeroed then set by its init |
| expression temps | fresh vregs |
| `if`/`while`/`do`/`for` | CFG edges + `Condition` |
| function call | `CALL` |

## Not in v1

`string` and string literals, floats, arrays/indices, pointers/references, objects/classes, closures, `?:`, optional chaining (`?.`), spread `...`, ranges, namespace/`::` resolution (dots are fine), `const`/immutability, implicit widening, and the stdlib (`std.print` comes with strings). The `examples/helloworld.erax` file is aspirational on `string`.