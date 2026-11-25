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
- **Post-name annotations and local inference.** Parameters and globals name their types. A local may omit its annotation when it has an initializer; the initializer then determines the type.
- **Assignment is a statement, not an expression.** `a = b = c` is forbidden, and an assignment has no value usable by an expression; compound forms like `a += 2` are the idiomatic mutation.
- **Required `;`.** Every terminating statement ends in `;`. No automatic semicolon insertion.
- A source file starts with its imports, followed by globals, functions, structures, and namespaces.

## Grammar

Terminals in quotes; `[]` optional, `{}` repeated, `|` choice. `INT`, `CHAR`, and `STRING` are integer, character, and string literals as lexed (see the tokenizer spec); an `INT` may carry a type suffix, which is part of the token (`5i64`, `0xFFu8`) — see `types.md#literals`.

```
program        = { import } { declaration } ;
import         = "import" "{" import-name { "," import-name } [ "," ] "}" "from" STRING ";" ;
import-name    = IDENT [ "as" IDENT ] ;
declaration    = [ "export" ] ( global | function | structure | namespace ) ;
namespace      = "namespace" IDENT "{" { declaration } "}" ;

global         = binding [ "mut" ] IDENT ":" type "=" expr ";" ;
binding        = "let" | "const" ;
function       = "fn" IDENT [ type-params ] "(" [ param { "," param } ] ")" [ ":" type ] block ;

structure      = "struct" IDENT [ type-params ] "{" { member } "}" ;
member         = [ "public" | "private" ] ( field | method | factory | destructor ) ;
field          = IDENT ":" type ";" ;
method         = [ "mut" | "static" ] "fn" IDENT "(" [ param { "," param } ] ")" [ ":" method-result ] block ;
method-result  = type | "ref" "mut" "?" type ;
factory        = "new" "(" [ param { "," param } ] ")" ":" type block ;
destructor     = "destructor" block ;
object-literal = "{" [ IDENT ":" expr { "," IDENT ":" expr } [ "," ] ] "}" ;

type-params    = "<" type-param { "," type-param } ">" ;
type-param     = IDENT [ "extends" capability { "&" capability } ] ;
capability     = "Ordered" | "Negatable" | "Arithmetic" | "Clonable" | "Movable" ;

param          = IDENT ":" type ;
type           = "i8" | "u8" | "i16" | "u16" | "i32" | "u32" | "i64" | "u64"
               | "bool" | "char" | "ptr" type | "ref" [ "mut" ] type
               | named-type | type-variable ;
named-type     = qualified-name [ "<" type { "," type } ">" ] ;
type-variable  = IDENT ;   // a name bound by the enclosing template's type-params

block          = "{" { block | statement } "}" ;        // a block is itself a statement

statement      = let-stmt | assign-stmt | expr-stmt | return-stmt
               | if-stmt | while-stmt | do-stmt | for-stmt
               | "break" ";" | "continue" ";" ;

let-stmt       = local-decl ";" ;
local-decl     = binding [ "mut" ] IDENT [ ":" type ] [ "=" expr ] ;
assign-stmt    = assignment ";" ;
assignment     = lvalue "=" expr | lvalue compound-op expr ;
compound-op    = "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^=" | "<<=" | ">>=" ;
expr-stmt      = expr ";" ;
return-stmt    = "return" [ expr ] ";" ;

if-stmt        = "if" "(" condition ")" block [ "else" ( if-stmt | block ) ] ;
while-stmt     = "while" "(" condition ")" block ;
do-stmt        = "do" block "while" "(" condition ")" ";" ;
for-stmt       = "for" "(" [ for-init ] ";" [ for-cond ] ";" [ for-step ] ")" block ;
for-init       = local-decl | assignment ;
for-cond       = condition ;
for-step       = assignment ;

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
unary          = ( "*" | "&" | "-" | "!" | "~" | "move" ) unary
               | "ref" [ "mut" [ "?" ] ] unary | postfix ;
postfix        = ( primary | call | type-call ) { "." IDENT [ "(" [ expr { "," expr } ] ")" ] | "[" expr "]" } ;
type-call      = named-type "." IDENT "(" [ expr { "," expr } ] ")" ;
call           = qualified-name "(" [ expr { "," expr } ] ")" ;
primitive-call = "primitive" IDENT "(" [ expr { "," expr } ] ")" ;
qualified-name = IDENT { "." IDENT } ;
allocation     = "alloc" "<" type ">" "(" expr ")" ;
primary        = allocation | INT | CHAR | STRING | "true" | "false" | "null" | qualified-name | primitive-call | object-literal | "(" expr ")" ;

lvalue         = "*" expr | postfix ;
const-expr     = expr limited to literals, the unary `-`/`!`/`~`, arithmetic and
                 comparison operators, `as` casts, and visible scalar globals
                 initialized earlier;
```

Notes:

- Every non-void `primary` `IDENT` is a **read** of a variable or global. An `IDENT` naming a parameter is also a read.
- A `condition` is the grammar shape of every test (`if`, `while`, `do`'s trailing `while`, and `for`'s cond): an `expr` whose type must be `bool`. Which expressions are `bool` is a typing rule in `expressions.md`, so the grammar only pins the shape here.
- A call names its callee with a `qualified-name` followed by `(`: the leading segments resolve to a namespace and the last names a function or template inside it. Structures also provide type-qualified factories and receiverless methods, and object expressions provide instance methods. Thus `std.print(x)` calls the `print` in `std`, and a bare `print(x)` calls the `print` visible from the current namespace (`#namespaces`, `expressions.md#names`).
- Bracket indexing applies to raw pointers. Collections expose access through their declared methods.
- Scalar global initializers must be constant-foldable and contain no calls. Record globals may call factories/functions and initialize in declaration order. Globals cannot contain safe references. See `statements.md#globals`.
- Every program is compiled with `stdlib/std.erx` parsed first. Every module can use `std.…` directly ([Standard library](../std.md)).
- Object literals are checked additionally: they assemble exactly the factory's own record type and are legal only inside that factory. See `statements.md#structures`.
- The parser recognizes `?` after `ref mut` in conditional method results and borrows.
- Nested type argument closers are interpreted by the parser even when the tokenizer emits a maximal-munch `>>` or `>>>` run. The operator whitelist remains unchanged outside type-argument positions.
- A `primitive-call` is answered by the built-in primitive catalog, never by a declaration: `primitive` + `IDENT` + `(` reaches exactly that primitive and nothing else (`docs/primitive.md#calling`). Primitives live in their own namespace — a plain `call` never resolves to one, and a function may share a primitive's name.
- A `cast`'s right side is a `type`, not an expression: `x as i8 + 1` is `(x as i8) + 1` and `x + 1 as i32` is `x + (1 as i32)` (`expressions.md#precedence`). The conversion rules — which types may cast to which, and what wrapping means — are `types.md#casts`.
- `*` and `&` are prefix in unary position and binary (`*` multiply, `&` bitwise-and) wherever a binary operator is expected — the position decides. `*p` reads the cell `p` points at, `&x` is the address of the variable or global `x` (`expressions.md#typing-rules`); `*` in front of a `=`/compound-assign makes a **store through a pointer** — `*p = v;` — a statement form, see `statements.md#statements`.
- `ptr T` is the pointer-to-`T` type (`types.md#types`): `ptr i32`, `ptr ptr i32`. `ptr` is **contextual** — it is a type only in type position and remains an ordinary identifier everywhere else (`#reserved-words`). A pointer is one cell wide whatever it points at (`JIR.md#types`).
- A call to a **template** (`statements.md#templates`) deduces its type arguments from the argument types; v1 has **no explicit instantiation syntax** — `abs<u8>(x)` is not written, because `::` is not a token and `<`/`>` compose at parse, so free-function type arguments remain deduction-only. A `type-param` binds its name as a `type-variable` for the declaration's signature and body only.

## Namespaces

A **namespace** is a named scope for the top-level names — globals, functions, structures,
and nested namespaces — declared with `namespace NAME { … }` (`#grammar`,
`statements.md#namespaces`). Names are reached **qualified**: `std.print(x)` calls
the `print` declared in `std`, and a bare name searches the current namespace
outward to the root.

- A namespace body holds `global`, `function` (concrete declarations, overloads,
  and templates alike), `structure`, and `namespace` declarations. The body is a scope: a bare
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

## Modules

Each source file is a module with its own root scope. Imports appear before
all declarations, at file scope. `from` is contextual in an import clause.

```ts
import { parse, ParseError as ConfigError } from "./config.erx";
import { net } from "../shared/network.erx";
```

- The module specifier is a literal path starting with `./` or `../` and ending
  in `.erx`. It uses literal path characters and forward slash separators. Paths
  resolve from the directory of the importing source file after canonicalization.
  A compilation from stdin has no file to resolve relative imports against.
- An import creates a binding for each listed export in the importing file's
  root. `as` sets its local name. Duplicate root bindings are errors. Imported
  names are visible in that file's nested namespaces through enclosing-scope
  lookup. All modules have the implicit `std` root binding.
- A declaration is private to its module unless marked `export`. An exported
  namespace exposes its explicitly exported members, and the rule applies
  recursively to nested namespaces. Structure members use the usual access
  rules. Marking a function declaration `export` exposes its entire overload set
  and any template under the same name. Template bodies use the declaring
  module's scope. Imports bind the original functions, types, and globals,
  regardless of any aliases. An imported global retains the original binding's
  mutability and assignment rules.
- A resolved file is compiled once per program. Import cycles are errors.
  Global initialization visits dependencies before their importers, traversing
  imports in source order and initializing each file's globals in declaration
  order. The entry file supplies the program's [`main`](statements.md#functions).

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
| `let` | reassignable variable / global declaration |
| `const` | fixed variable / global binding |
| `mut` | permission to mutate an object independently of rebinding |
| `move` | explicitly transfer a value |
| `ref` | safe reference type or explicit borrowing |
| `struct` | structure declaration |
| `public` `private` | structure member visibility |
| `destructor` | additional resource cleanup |
| `namespace` | named scope for top-level declarations |
| `import`, `export` | module bindings and visibility |
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
| `null` | Contextual raw pointer literal |
| `false` | `bool` constant |
| `i8` `u8` `i16` `u16` `i32` `u32` `i64` `u64` `bool` `char` | type names |

Type names, `true`, and `false` are reserved because they carry their meaning in type and literal position.

`string`, `std`, and `text` are ordinary identifiers. `ptr` is contextual in type position (`#grammar`). The standard library type for strings is `std.string` (see `types.md#literals`). The auto-imported library claims the name `std` at each module root (`docs/std.md`). The capability names `Ordered`, `Negatable`, `Arithmetic`, `Clonable`, and `Movable` have special meaning after `extends` or `&` in a `type-param`. The entry module's root `main` follows the [entry point rules](statements.md#functions).

## Semantics

- **Value semantics and explicit borrowing.** Scalar values copy their bits; record lvalues copy through `Clonable`, and `move` transfers ownership explicitly. `ref` and `ref mut` create checked references; reference values copy the reference, not its target. Raw `ptr` remains an unchecked ownership mechanism underneath. See `types.md#references` and `statements.md#structures`.
- **Scoping.** A `block` (and a `for`'s init) opens a scope; enclosing scopes are visible, inner names shadow outer ones. A local name is in scope from its declaration to the end of its block. Globals follow module and namespace visibility. A global initializer can reference globals from an initialized dependency or globals declared earlier in its own file, subject to the initializer's typing rules. A `namespace` is a scope for top-level names (`#namespaces`).
- **Definite assignment.** Every read of a variable is preceded, on every execution path, by a write (`let` initializer or an assignment). Parameters start initialized; `move` consumes their value just as it consumes a local. Scalar globals have constant initializers; record globals initialize through ordinary expressions. This is enforced statically; it never produces runtime behavior. Pointer reads and stores do not reach the check but cannot defeat it: `&x` is itself a *read*, so the address of an as-yet-unassigned local is the same `read before it is assigned` error as any other read, and a `*p = v` store writes a cell the checker cannot track, so it never counts as the write that satisfies a name.
- **Bindings and moves.** `const` prevents rebinding, not moving. `let` may be initialized again after `move`; `const` may not. A local without an annotation needs an initializer, and every `const` needs one. Initialization state is checked over the CFG, including loop back edges. See `statements.md#bindings-and-moves`.
- **RAII.** Initialized owners are destroyed on ordinary scope exits, returns, breaks, and continues. Local order is reverse initialization; field order is reverse declaration after the user destructor. Traps and immediate termination do not unwind. See `statements.md#destruction`.
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
| `ref T` / `ref mut T` | a pointer to the lowered target cell; safety is checked before lowering |
| structure value | one owned heap handle; fields occupy one cell each, nested owned records use handles |
| `p[index]` | `OFFSET` with `LOAD` or `STORE` |
| `p + index` | `OFFSET`, checked within the same allocation |
| `alloc<T>(count)` | `PRIM alloc_cells` with a typed result |
| structure member | `OFFSET` plus typed `LOAD` / `STORE` |
| factory / method / destructor | ordinary functions and `CALL`; cleanup uses CFG edges |
| local / param | a vreg at a frame offset |
| global | a `@` global, zeroed then set by its init |
| expression temps | fresh vregs |
| `&&` `\|\|` `^^` | `AND` `OR` `XOR` on `bool` (both operands always evaluated) |
| `move x` | `MOV` to a fresh result cell; consumption is a frontend-only check |
| `&x` | `ADDR` — the address of `x`'s cell (a local, a param, or a global) |
| `*p` | `LOAD` — the cell `p` points at, in the pointee's type |
| `*p = v` / `*p += v` | `STORE` (a compound is `LOAD` → the op → `STORE`) |
| `if`/`while`/`do`/`for` | CFG edges + `Condition` |
| function call | `CALL` |
| template call | a `CALL` to the function that instantiation monomorphized into — the IR has no templates (`statements.md#templates`) |
| `primitive p(...)` | `PRIM` |
| `x as T` | `CAST` — except a to-`bool` cast, which is a compare against `0` (JIR has no `bool` type) |
