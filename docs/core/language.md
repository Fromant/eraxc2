# Core language — erax

Source-level contract of the erax language: grammar, types, and semantics. The docs under `docs/core/` describe **v1**.

- Grammar, operator set, reserved words, semantics: this file.
- Types and literals: `types.md`.
- Expressions and precedence: `expressions.md`.
- Statements, functions, globals: `statements.md`.
- Primitives — the backend-provided `primitive` calls: `docs/primitive.md`.
- The standard library (auto-imported `stdlib/std.erx`): `docs/std.md`.
- Lexing (tokens, trivia, literals' lexical shape) is specified in `docs/frontend/tokenizer.md`.
- Lowering and runtime behavior of the machine level are in `docs/middleend/JIR.md`; the source contract here is the part the parser must enforce and can reject.
- Roadmap and versioning live in `TODO.md`.

## Style

- **Post-name type syntax.** Types are written after the name, everywhere: `let t: i32 = 123;`, params `fn foo(x: i32, y: bool)`, return `fn main(): i32 { … }`.
- **Explicit types only.** Every `let` and every param names its type; nothing is inferred.
- **Assignment is a statement, not an expression.** `a = b = c` is illegal (an assignment's value is never usable); compound forms like `a += 2` are the idiomatic mutation.
- **Required `;`.** Every terminating statement ends in `;`. No automatic semicolon insertion.
- A source file holds only **global declarations and function declarations** (top level). Everything else lives inside functions.

## Grammar

Terminals in quotes; `[]` optional, `{}` repeated, `|` choice. `INT`/`CHAR` are integer/character literals as lexed (see the tokenizer spec); an `INT` may carry a type suffix, which is part of the token (`5i64`, `0xFFu8`) — see `types.md#literals`.

```
program        = { global | function } ;

global         = "let" IDENT ":" type "=" const-expr ";" ;
function       = "fn" IDENT "(" [ param { "," param } ] ")" [ ":" type ] block ;

param          = IDENT ":" type ;
type           = "i8" | "u8" | "i16" | "u16" | "i32" | "u32" | "i64" | "u64"
               | "bool" | "char" ;

block          = "{" { block | statement } "}" ;        // a block is itself a statement

statement      = let-stmt | assign-stmt | expr-stmt | return-stmt
               | if-stmt | while-stmt | do-stmt | for-stmt
               | "break" ";" | "continue" ";" ;

let-stmt       = "let" IDENT ":" type [ "=" expr ] ";" ;
assign-stmt    = lvalue assign-op expr ";" ;
expr-stmt      = expr ";" ;
return-stmt    = "return" [ expr ] ";" ;

if-stmt        = "if" "(" condition ")" block [ "else" ( if-stmt | block ) ] ;
while-stmt     = "while" "(" condition ")" block ;
do-stmt        = "do" block "while" "(" condition ")" ";" ;
for-stmt       = "for" "(" [ for-init ] ";" [ for-cond ] ";" [ for-step ] ")" block ;
for-init       = "let" IDENT ":" type [ "=" expr ] | lvalue assign-op expr ;
for-cond       = condition ;
for-step       = expr ;

condition      = expr ;    // must be `bool` — see expressions.md

expr           = or ;
or             = logxor { "||" logxor } ;
logxor         = and { "^^" and } ;
and            = bitor { "&&" bitor } ;
bitor          = bitxor { "|" bitxor } ;
bitxor         = bitand { "^" bitand } ;
bitand         = equality { "&" equality } ;
equality       = relational { ( "==" | "!=" ) relational } ;
relational     = shift { ( "<" | "<=" | ">" | ">=" ) shift } ;
shift          = additive { ( "<<" | ">>" ) additive } ;
additive       = multiplicative { ( "+" | "-" ) multiplicative } ;
multiplicative = cast { ( "*" | "/" | "%" ) cast } ;
cast           = unary { "as" type } ;
unary          = ( "-" | "!" | "~" ) unary | postfix ;
postfix        = primary | call ;
call           = IDENT "(" [ expr { "," expr } ] ")" ;
primitive-call = "primitive" IDENT "(" [ expr { "," expr } ] ")" ;
primary        = INT | CHAR | "true" | "false" | IDENT | primitive-call | "(" expr ")" ;

lvalue         = IDENT ;
const-expr     = expr limited to literals, the unary `-`/`!`/`~`, arithmetic and
                 comparison operators, `as` casts, and references to globals
                 declared earlier in the file;
```

Notes:

- Every non-void `primary` `IDENT` is a **read** of a variable or global. An `IDENT` naming a parameter is also a read.
- A `condition` is the grammar shape of every test (`if`, `while`, `do`'s trailing `while`, and `for`'s cond): an `expr` whose type must be `bool`. Which expressions are `bool` is a typing rule in `expressions.md`, so the grammar only pins the shape here.
- A call names its callee with a bare `IDENT` followed by `(`. A `.` after a name is a syntax error:
  v1 has no member access and no namespaces (`TODO.md` carries dot namespaces as future work).
- `[` `]` lex but are not part of v1 (arrays are future work); using them is a syntax error.
- `const-expr` is enforced additionally: it must be constant-foldable and contain no calls (a `primitive` call is a call).
- Every program is compiled with `stdlib/std.erx` parsed first (auto-import), so std's declarations are part of the program and need no import of their own (`docs/std.md`).
- A `primitive-call` is answered by the built-in primitive catalog, never by a declaration: `primitive` + `IDENT` + `(` reaches exactly that primitive and nothing else (`docs/primitive.md#calling`). Primitives live in their own namespace — a plain `call` never resolves to one, and a function may share a primitive's name.
- A `cast`'s right side is a `type`, not an expression: `x as i8 + 1` is `(x as i8) + 1` and `x + 1 as i32` is `x + (1 as i32)` (`expressions.md#precedence`). The conversion rules — which types may cast to which, and what wrapping means — are `types.md#casts`.

## Operator set

A tokenizer `Operator` token is legal only if its full text is one of:

```
+ - * / %         arithmetic
<< >>             shifts
< <= > >=         relational
== !=             equality
& | ^             bitwise
|| ^^ &&          logical
= += -= *= /= %= &= |= ^= <<= >>=    assignment (statement-level)
! ~               unary (single-character)
```

Any other operator text — `===`, `++`, `--`, `**`, `=>`, `??`, `&&&`, … — is a compile-time `unknown operator` error. (The tokenizer's maximal munch guarantees ambiguity-free lexing: `a+++++b` is one token run and is rejected as an unknown operator.)

## Reserved words

Every keyword is reserved: it cannot be used as an `IDENT` (declaration or reference). The complete list:

| Keyword | Role |
| ------- | ---- |
| `fn` | function declaration |
| `let` | variable / global declaration |
| `if` | conditional block |
| `else` | alternate branch of `if` |
| `while` | loop |
| `do` | do-while loop |
| `for` | C-shaped loop |
| `break` | exit the nearest loop |
| `continue` | next iteration of the nearest loop |
| `return` | return from the current function |
| `primitive` | call a backend primitive |
| `as` | cast a value to another type |
| `true` | `bool` constant |
| `false` | `bool` constant |
| `i8` `u8` `i16` `u16` `i32` `u32` `i64` `u64` `bool` `char` | type names |

Type names, `true`, and `false` are reserved because they carry their meaning in type and literal position.

`string`, `std`, `text`, … are ordinary identifiers, not reserved words: they may be used as names. Writing `string` in *type* position is a forward-looking error (see `types.md#literals`). One name carries its meaning without being reserved: `main` is the entry point — a rule on the name, see `statements.md#functions`. It may not be given a declaration of its own, and it does not stop the tokenizer.

## Semantics

- **By-value, no references.** Variables hold values; there are no pointers, aliasing, references, or lvalue-producing operators.
- **Scoping.** A `block` (and a `for`'s init) opens a scope; enclosing scopes are visible, inner names shadow outer ones. A name is in scope from its declaration to the end of its block. Globals are visible everywhere, including inside functions and other global initializers — but a global initializer may reference only globals **declared earlier** in the file.
- **Definite assignment.** Every read of a variable is preceded, on every execution path, by a write (`let` initializer or an assignment). Parameters and globals always have a value. This is enforced statically; it never produces runtime behavior.
- **Runtime arithmetic.** All integer arithmetic wraps (two's complement), exactly as the IR defines. Division/modulo truncate toward zero; `x / 0` (and `imin / -1`) terminate the process — no UB. Shift counts are masked to the operand width (`count & (width-1)`), so no shift is undefined.
- **Division by zero is a run-time error, not a compile-time one.** Only a *constant* zero divisor can be caught while reading: `a / 0` is rejected, because a constant expression has to have an answer. With a variable, `let b: i32 = 0; a / b;` compiles and traps when it runs — the same way the machine's own divide instruction does. This is a documented, uniform rule, not undefined behaviour; the error surfaces wherever the trap does, and a backend may add a message to it. See `types.md#constants` for why a constant is checked and a runtime value is not.
- **Logical operators are eager.** `&&`, `||`, and `^^` always evaluate both operands; there is no short-circuiting.
- **No undefined behavior.** Anything the rules don't define — reading an undefined variable, wrong type, mismatched arity, unknown operator — is a compile-time error, not runtime behavior.
- **Determinism.** One evaluation order (left-to-right for the given associativity/precedence), single-threaded, no implicit state; the same program behaves identically on every backend.

## Machine-level contract

The language lowers 1:1 to JIR:

| Source | JIR |
| ------ | --- |
| `bool` | `i8` |
| `char` | `u8` |
| `i8`…`u64` | same |
| local / param | a vreg at a frame offset |
| global | a `@` global, zeroed then set by its init |
| expression temps | fresh vregs |
| `&&` `\|\|` `^^` | `AND` `OR` `XOR` on `bool` (both operands always evaluated) |
| `if`/`while`/`do`/`for` | CFG edges + `Condition` |
| function call | `CALL` |
| `primitive p(...)` | `PRIM` |
| `x as T` | `CAST` — except a to-`bool` cast, which is a compare against `0` (JIR has no `bool` type) |