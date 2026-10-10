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
program        = { global | function | namespace } ;
namespace      = "namespace" IDENT "{" { global | function | namespace } "}" ;

global         = "let" IDENT ":" type "=" const-expr ";" ;
function       = "fn" IDENT [ type-params ] "(" [ param { "," param } ] ")" [ ":" type ] block ;

type-params    = "<" type-param { "," type-param } ">" ;
type-param     = IDENT [ "extends" capability { "&" capability } ] ;
capability     = "Ordered" | "Negatable" | "Arithmetic" ;

param          = IDENT ":" type ;
type           = "i8" | "u8" | "i16" | "u16" | "i32" | "u32" | "i64" | "u64"
               | "bool" | "char" | "ptr" type | type-variable ;
type-variable  = IDENT ;   // a name bound by the enclosing template's type-params

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
unary          = ( "*" | "&" | "-" | "!" | "~" ) unary | postfix ;
postfix        = primary | call ;
call           = qualified-name "(" [ expr { "," expr } ] ")" ;
primitive-call = "primitive" IDENT "(" [ expr { "," expr } ] ")" ;
qualified-name = IDENT { "." IDENT } ;
primary        = INT | CHAR | "true" | "false" | qualified-name | primitive-call | "(" expr ")" ;

lvalue         = "*" expr | qualified-name ;
const-expr     = expr limited to literals, the unary `-`/`!`/`~`, arithmetic and
                 comparison operators, `as` casts, and references to globals
                 declared earlier in the file;
```

Notes:

- Every non-void `primary` `IDENT` is a **read** of a variable or global. An `IDENT` naming a parameter is also a read.
- A `condition` is the grammar shape of every test (`if`, `while`, `do`'s trailing `while`, and `for`'s cond): an `expr` whose type must be `bool`. Which expressions are `bool` is a typing rule in `expressions.md`, so the grammar only pins the shape here.
- A call names its callee with a `qualified-name` followed by `(`: the leading segments resolve to a namespace and the last names a function or template inside it. Thus `std.print(x)` calls the `print` in `std`, and a bare `print(x)` calls the `print` visible from the current namespace (`#namespaces`, `expressions.md#names`).
- `[` `]` lex but are not part of v1 (arrays are future work); using them is a syntax error.
- `const-expr` is enforced additionally: it must be constant-foldable and contain no calls (a `primitive` call is a call).
- Every program is compiled with `stdlib/std.erx` parsed first (auto-import), so std's declarations are part of the program and need no import of their own (`docs/std.md`).
- A `primitive-call` is answered by the built-in primitive catalog, never by a declaration: `primitive` + `IDENT` + `(` reaches exactly that primitive and nothing else (`docs/primitive.md#calling`). Primitives live in their own namespace — a plain `call` never resolves to one, and a function may share a primitive's name.
- A `cast`'s right side is a `type`, not an expression: `x as i8 + 1` is `(x as i8) + 1` and `x + 1 as i32` is `x + (1 as i32)` (`expressions.md#precedence`). The conversion rules — which types may cast to which, and what wrapping means — are `types.md#casts`.
- `*` and `&` are prefix in unary position and binary (`*` multiply, `&` bitwise-and) wherever a binary operator is expected — the position decides. `*p` reads the cell `p` points at, `&x` is the address of the variable or global `x` (`expressions.md#typing-rules`); `*` in front of a `=`/compound-assign makes a **store through a pointer** — `*p = v;` — a statement form, see `statements.md#statements`.
- `ptr T` is the pointer-to-`T` type (`types.md#types`): `ptr i32`, `ptr ptr i32`. `ptr` is **contextual**, like `string` — it is a type only in type position and remains an ordinary identifier everywhere else (`#reserved-words`). A pointer is one cell wide whatever it points at (`JIR.md#types`).
- A call to a **template** (`statements.md#templates`) deduces its type arguments from the argument types; v1 has **no explicit instantiation syntax** — `abs<u8>(x)` is not written, because `::` is not a token and `<`/`>` compose at parse, so naming the arguments is future work (`TODO.md`). A `type-param` binds its name as a `type-variable` for the declaration's signature and body only.

## Namespaces

A **namespace** is a named scope for the top-level names — globals, functions,
and nested namespaces — declared with `namespace NAME { … }` (`#grammar`,
`statements.md#namespaces`). Names are reached **qualified**: `std.print(x)` calls
the `print` declared in `std`, and a bare name searches the current namespace
outward to the root.

- A namespace body holds `global`, `function` (concrete declarations, overloads,
  and templates alike), and `namespace` declarations. The body is a scope: a bare
  `f(...)` or `g` inside it refers to that namespace's `f`/`g` first, then its
  enclosing namespaces.
- A qualified name's first segment resolves as a name searches its enclosing
  chain; each later segment is a **direct member** of the namespace before it.
  So `a.b.c` is `c` in the `b` nested in `a`, and a qualified name never keeps
  searching outward after its first segment.
- One kind of thing per name, per namespace: a namespace may not share a name
  with a global, a function, or another namespace it contains. Root and inner
  namespaces may reuse a name across the boundary — a member shadows an
  enclosing namespace's name of the same name.
- A namespace is declared once; reopening `std` a second time is the same
  duplicate-declaration error.
- A namespace is not a value: naming one anywhere but as a qualified prefix is
  an error. Locals are never qualified — only top-level names live in
  namespaces.
- The standard library is the auto-imported `namespace std` (`docs/std.md`): its
  functions are part of every program and reached as `std.…`.

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

Any other operator text — `===`, `++`, `--`, `**`, `=>`, `??`, `&&&`, … — is a compile-time `unknown operator` error. (The tokenizer's maximal munch guarantees ambiguity-free lexing: `a+++++b` is one token run and is rejected as an unknown operator.) This includes a nested deref: `**p` is the single run `**`, so it is rejected too — a pointer to a pointer is dereffed one level at a time, `*(*p) = v;` (`expressions.md#typing-rules`).

## Reserved words

Every keyword is reserved: it cannot be used as an `IDENT` (declaration or reference). The complete list:

| Keyword | Role |
| ------- | ---- |
| `fn` | function declaration |
| `let` | variable / global declaration |
| `namespace` | named scope for top-level declarations |
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
| `extends` | a type parameter's capability bound |
| `true` | `bool` constant |
| `false` | `bool` constant |
| `i8` `u8` `i16` `u16` `i32` `u32` `i64` `u64` `bool` `char` | type names |

Type names, `true`, and `false` are reserved because they carry their meaning in type and literal position.

`string`, `std`, `text`, … are ordinary identifiers, not reserved words: they may be used as names. `ptr` is the same kind of contextual name: it is a type in type position (`#grammar`) and an ordinary identifier elsewhere, so a variable may be called `ptr`. Writing `string` in *type* position is a forward-looking error (see `types.md#literals`). `std` is claimed at the root by the auto-imported library (`docs/std.md`), so a program declaring its own top-level `std` is a duplicate-declaration error. The capability names `Ordered`, `Negatable`, `Arithmetic` are ordinary identifiers too — they mean something only after `extends`/`&` in a `type-param`, and a template's type-parameter names are ordinary outside the declaration (`statements.md#templates`). One name carries its meaning without being reserved: `main` is the entry point — a rule on the name, see `statements.md#functions`. It may not be given a declaration of its own, and it does not stop the tokenizer.

## Semantics

- **By-value; pointers are values, not references.** Variables hold values, copied on assignment and call; there are no reference bindings and no lvalue-producing binary operators. A **pointer** is one of those values — the address of a cell, named with `&` and followed with `*` (`types.md#types`, `expressions.md#typing-rules`). Pointers are plain data: they copy, they pass by value, they compare equal iff they name the same cell, and no capability holds for them (`types.md#capabilities`).
- **Scoping.** A `block` (and a `for`'s init) opens a scope; enclosing scopes are visible, inner names shadow outer ones. A name is in scope from its declaration to the end of its block. Globals are visible everywhere, including inside functions and other global initializers — but a global initializer may reference only globals **declared earlier** in the file. A `namespace` is a scope too, for top-level names only (`#namespaces`).
- **Definite assignment.** Every read of a variable is preceded, on every execution path, by a write (`let` initializer or an assignment). Parameters and globals always have a value. This is enforced statically; it never produces runtime behavior. Pointer reads and stores do not reach the check but cannot defeat it: `&x` is itself a *read*, so the address of an as-yet-unassigned local is the same `read before it is assigned` error as any other read, and a `*p = v` store writes a cell the checker cannot track, so it never counts as the write that satisfies a name.
- **Runtime arithmetic.** All integer arithmetic wraps (two's complement), exactly as the IR defines. Division/modulo truncate toward zero; `x / 0` (and `imin / -1`) terminate the process — no UB. Shift counts are masked to the operand width (`count & (width-1)`), so no shift is undefined.
- **Division by zero is a run-time error, not a compile-time one.** Only a *constant* zero divisor can be caught while reading: `a / 0` is rejected, because a constant expression has to have an answer. With a variable, `let b: i32 = 0; a / b;` compiles and traps when it runs — the same way the machine's own divide instruction does. This is a documented, uniform rule, not undefined behaviour; the error surfaces wherever the trap does, and a backend may add a message to it. See `types.md#constants` for why a constant is checked and a runtime value is not.
- **Pointer misuse is a run-time error, not a compile-time one.** `&x` needs a cell to name, but a pointer can be copied to anywhere and the machine cannot know every read targets its cell: `*p` or `*p = v` through a pointer that names nothing — the null pointer, an address freed or never allocated, a cell in a frame that has returned — terminates the process with the same machinery a divide does (`JIR.md#runtime-semantics`). The parser still rejects what it can see: `&` of a constant or expression is a compile error, and a deref whose operand is not a pointer is a type error (`expressions.md#typing-rules`).
- **Logical operators are eager.** `&&`, `||`, and `^^` always evaluate both operands; there is no short-circuiting.
- **No undefined behavior.** Anything the rules don't define — reading an undefined variable, wrong type, mismatched arity, unknown operator — is a compile-time error, not runtime behavior.
- **Determinism.** One evaluation order (left-to-right for the given associativity/precedence), single-threaded, no implicit state; the same program behaves identically on every backend. Pointer **identity** is part of this — equal iff the same cell — but the *specific* address a cell names is the executor's business, not the IR's, so a program never depends on it: `&x == &y` means what it says, while a program that turns addresses into integers (`p as u64`) and prints or further compares them leaves the deterministic contract (`TODO.md#conformance`).

## Machine-level contract

The language lowers 1:1 to JIR:

| Source | JIR |
| ------ | --- |
| `bool` | `i8` |
| `char` | `u8` |
| `i8`…`u64` | same |
| `ptr T` | `ptr T` (a pointer is one word, the pointee lowered) |
| local / param | a vreg at a frame offset |
| global | a `@` global, zeroed then set by its init |
| expression temps | fresh vregs |
| `&&` `\|\|` `^^` | `AND` `OR` `XOR` on `bool` (both operands always evaluated) |
| `&x` | `ADDR` — the address of `x`'s cell (a local, a param, or a global) |
| `*p` | `LOAD` — the cell `p` points at, in the pointee's type |
| `*p = v` / `*p += v` | `STORE` (a compound is `LOAD` → the op → `STORE`) |
| `if`/`while`/`do`/`for` | CFG edges + `Condition` |
| function call | `CALL` |
| template call | a `CALL` to the function that instantiation monomorphized into — the IR has no templates (`statements.md#templates`) |
| `primitive p(...)` | `PRIM` |
| `x as T` | `CAST` — except a to-`bool` cast, which is a compare against `0` (JIR has no `bool` type) |