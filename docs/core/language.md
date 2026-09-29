# Core language — erax

Source-level contract of the erax language: grammar, types, and semantics. The docs under `docs/core/` describe **v1**.

- Grammar, operator set, reserved words, semantics: this file.
- Types and literals: `types.md`.
- Expressions and precedence: `expressions.md`.
- Statements, functions, globals: `statements.md`.
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

Terminals in quotes; `[]` optional, `{}` repeated, `|` choice. `INT`/`CHAR` are integer/character literals as lexed (see the tokenizer spec).

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
- A `condition` is the grammar shape of every test (`if`, `while`, `do`'s trailing `while`, and `for`'s cond): an `expr` whose type must be `bool`. Which expressions are `bool` is a typing rule in `expressions.md`, so the grammar only pins the shape here.
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
| `true` | `bool` constant |
| `false` | `bool` constant |
| `i8` `u8` `i16` `u16` `i32` `u32` `i64` `u64` `bool` `char` | type names |

Type names, `true`, and `false` are reserved because they carry their meaning in type and literal position.

`string`, `std`, `print`, `text`, … are ordinary identifiers, not reserved words: they may be used as names. Writing `string` in *type* position is a forward-looking error (see `types.md#literals`). `main` has the same status — the entry point is a rule on that name, see `statements.md#functions`.

## Semantics

- **By-value, no references.** Variables hold values; there are no pointers, aliasing, references, or lvalue-producing operators.
- **Scoping.** A `block` (and a `for`'s init) opens a scope; enclosing scopes are visible, inner names shadow outer ones. A name is in scope from its declaration to the end of its block. Globals are visible everywhere, including inside functions and other global initializers — but a global initializer may reference only globals **declared earlier** in the file.
- **Definite assignment.** Every read of a variable is preceded, on every execution path, by a write (`let` initializer or an assignment). Parameters and globals always have a value. This is enforced statically; it never produces runtime behavior.
- **Runtime arithmetic.** All integer arithmetic wraps (two's complement), exactly as the IR defines. Division/modulo truncate toward zero; `x / 0` (and `imin / -1`) terminate the process — no UB. Shift counts are masked to the operand width (`count & (width-1)`), so no shift is undefined.
- **No undefined behavior.** Anything the rules don't define — reading an undefined variable, wrong type, mismatched arity, unknown operator — is a compile-time error, not runtime behavior.
- **Determinism.** One evaluation order (left-to-right for the given associativity/precedence), single-threaded, no implicit state; the same program behaves identically on every backend.

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