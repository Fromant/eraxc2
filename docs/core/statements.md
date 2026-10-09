# Statements, functions, globals

The statement grammar is in `language.md#grammar`; this file gives each statement's rules.

## Statements

- **`let`.** Declares a variable in the current scope; the name must not already exist in that scope (shadowing an outer scope is allowed). Without an initializer, the variable must receive a **guaranteed assignment before any read** (definite assignment — a compile-time check over all paths; see `language.md#semantics`). An initializer that reads the variable itself (`let x: i32 = x;`) is an error.
- **Assignment.** `lvalue op expr`; `lvalue` is a variable or global declared in an enclosing (or the same) scope. `a += 1` reads `a`, writes it, and is equivalent to `a = a + 1` (so `a` must already be definite-assigned). Assignments are statements, not expressions — `a = b = c` and `x = (a += 2)` are parse errors.
- **Expression statements.** Any expression may be a statement; its value is discarded (`x;`, `5;`, `foo();`). Assignment forms are the idiomatic way to get a mutation.
- **`primitive`.** A `primitive` call is an ordinary expression statement like any other (`primitive print_char('a');`), with its typing in `expressions.md#typing-rules` and its contracts in `docs/primitive.md`. Two of them, `exit` and `abort`, **end the flow of their block exactly like `return`**: nothing after them runs, and in a non-void function a path that ends in one satisfies the reaching-return check without a `return`.
- **`return`.** In a non-void function, `return expr` on every path (reaching-return check) with `expr` of exactly the return type. In a void function, `return;` or falling off the end. `return expr` in a void function is an error.
- **`if` / `else`.** The condition (`condition` in the grammar) must be `bool`. `else` binds to the nearest `if`; `else if` is spelling for a nested `if`.
- **`while` / `do` / `for`.** Conditions (`condition` in the grammar) are `bool`. `for` is C-shaped: `for (init; cond; step)` — *init* is a `let` (scoped to the loop; `= expr` is optional, as with any `let`) or an assignment, *cond* is `bool`, *step* is any expression. All three clauses are optional (a bare `for(;;)` is an infinite loop).
- **`break` / `continue`.** Legal only inside a loop body (dynamically nested in `while`/`do`/`for`); `break` in a non-loop is an error. In the example, `break` inside an `if` inside a `for` is fine.
- **Unreachable code is dropped, not rejected.** A statement that cannot execute — one after a `return` or after a terminating `primitive exit`/`abort`, or one in a loop whose body never falls through — is still parsed and fully checked, but it produces no code: the IR drops the blocks no edge can reach (see `docs/middleend/JIR.md`).

## Functions

- Parameters are `IDENT : type`, comma-separated; the return type is `[ ":" type ]` — absent means `void`.
- A function body is a single `block`. Recursion is ordinary: every call gets its own frame (see `docs/middleend/JIR.md`).
- **Declaration order is free for functions.** A function may be called before it is declared, and may call itself. Only global initializers are restricted, to globals declared earlier.
- **A function is identified by its name plus its parameter types, and may be overloaded.** One name may carry several declarations — an **overload set** — as long as no two share the same parameter types; the return type is not part of the identity, so two declarations may not differ only in it. Declaring the same signature twice under one name is an error — `'f' is already declared`, the message a duplicate global gets too. A call picks its declaration by exact argument types, with no conversions between candidates (`expressions.md#typing-rules`).
- Arguments are passed **by value** and evaluate left to right; the callee reads copies in its own frame.
- A function sees only its own parameters/locals and the globals — never another function's locals.
- **Entry point.** The program must declare exactly one `fn main(): i32` (no parameters). Its return value is the process exit code — as is the `code` given to `primitive exit` if the program terminates that way (`docs/primitive.md`) — and it runs after all global initializers have run. `main` is a rule on the *name* (it is not a reserved word); no other function may be named `main`.

## Templates

A function may declare **type parameters** — `fn name<T extends Ordered & Negatable>(…)` — which makes it a **template**. A type parameter is an ordinary name bound inside the declaration and usable wherever a type is (parameter and return types, `let` annotations, cast targets):

```
fn abs<T extends Ordered & Negatable>(val: T): T {
    if (val < 0) {
        return -val;
    }
    return val;
}
```

- A type parameter may carry a **bound**: one or more capabilities joined by `&` (`types.md#capabilities`). The bound is what the template promises about `T`; a type that does not satisfy it cannot instantiate the template, and the call is rejected (`expressions.md#typing-rules`).
- **Every type parameter must appear in a parameter type.** A parameter list that never names `T` gives deduction nothing to work from, so `fn make<T>(): T` is an error at the declaration — `type parameter 'T' of 'make' is not in the parameters`.
- **A name carries at most one template** (v1), and may carry concrete declarations beside it. On a call, a concrete declaration that matches the argument types exactly is chosen first; the template is the fallback (`expressions.md#typing-rules`). This is what lets `print<T>` sit beside an ordinary `print(char)`.
- **A template is monomorphized.** There are no templates in the IR: each distinct set of type arguments **instantiates** an ordinary function (same name, concrete parameter and return types), and calls lower to a `CALL` to it (`language.md#machine-level-contract`). An instantiation is reused for every call that deduces the same type arguments, so a template that calls itself with the same types is ordinary recursion.
- **The body is checked per instantiation**, with `T` standing for the deduced type. The bound is a promise made at the call, not a proof that the body stays inside it: an operation the bound did not name is rejected when that instantiation is compiled. (Checking the body once, against the bound, is future work — `TODO.md`.)
- A chain of instantiations that keeps demanding *new* types does not terminate, and is a compile-time error.

## Globals

A `global` is a top-level `let` with a required, explicitly written initializer:

```
let counter: i64 = 0;
let multiplier: i32 = 2 + 3;
```

There is **no implicit zero**: every global must be given its starting value in the source. Backends still zero the storage before running init code (that is an implementation detail of JIR); the language contract is that a global's value is the one its initializer wrote, in declaration order, before `main` runs. Globals are writable at runtime (`counter = counter + 1;`).