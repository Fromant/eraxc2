# Statements, functions, globals

The statement grammar is in `language.md#grammar`; this file gives each statement's rules.

## Statements

- **`let`.** Declares a variable in the current scope; the name must not already exist in that scope (shadowing an outer scope is allowed). Without an initializer, the variable must receive a **guaranteed assignment before any read** (definite assignment — a compile-time check over all paths; see `language.md#semantics`). An initializer that reads the variable itself (`let x: i32 = x;`) is an error.
- **Assignment.** `lvalue op expr`; `lvalue` is a variable or global declared in an enclosing (or the same) scope. `a += 1` reads `a`, writes it, and is equivalent to `a = a + 1` (so `a` must already be definite-assigned). Assignments are statements, not expressions — `a = b = c` and `x = (a += 2)` are parse errors.
- **Expression statements.** Any expression may be a statement; its value is discarded (`x;`, `5;`, `foo();`). Assignment forms are the idiomatic way to get a mutation.
- **`return`.** In a non-void function, `return expr` on every path (reaching-return check) with `expr` of exactly the return type. In a void function, `return;` or falling off the end. `return expr` in a void function is an error.
- **`if` / `else`.** Condition must be `bool`. `else` binds to the nearest `if`; `else if` is spelling for a nested `if`.
- **`while` / `do` / `for`.** Conditions are `bool`. `for` is C-shaped: `for (init; cond; step)` — *init* is a `let` (scoped to the loop; `= expr` is optional, as with any `let`) or an assignment, *cond* is `bool`, *step* is any expression. All three clauses are optional (a bare `for(;;)` is an infinite loop).
- **`break` / `continue`.** Legal only inside a loop body (dynamically nested in `while`/`do`/`for`); `break` in a non-loop is an error. In the example, `break` inside an `if` inside a `for` is fine.

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