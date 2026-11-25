# Statements, functions, globals

The statement grammar is in `language.md#grammar`; this file gives each statement's rules.

## Statements

- **`let` / `const`.** Declare a variable in the current scope; the name must not already exist in that scope (shadowing an outer scope is allowed). Without an initializer, the variable must receive a **guaranteed assignment before any read** (definite assignment — a compile-time check over all paths; see `language.md#semantics`). An initializer that reads the variable itself (`let x: i32 = x;`) is an error.
- **Assignment.** `lvalue op expr`; `lvalue` is a variable or global declared in an enclosing (or the same) scope — or a **cell read through a pointer**, `*p` (`lvalue = "*" expr | qualified-name` in the grammar). `a += 1` reads `a`, writes it, and is equivalent to `a = a + 1` (so `a` must already be definite-assigned). A **deref assignment** is the same shape on the pointed-to cell: `*p = v` writes the pointee (`v` typed against it — a lone literal takes the pointee's type), and `*p += 2` is the read-modify-write, `LOAD`, the operator, `STORE` back through the pointer (`expressions.md#typing-rules`). A compound store needs an integer pointee, like every compound form. Assignments are statements, not expressions. Assignment chains such as `a = b = c` and `a = b += c` are forbidden. `x = (a += 2)` is a parse error, and so is `*p = 1` in an expression position. The definite-assignment check does not track pointers, and it does not need to: `&x` is a *read*, so it demands `x` definite-assigned already, and a pointer store never counts as the write that satisfies a name — `let x: i32; let p: ptr i32 = &x;` is rejected at the `&`.
- **Expression statements.** Any expression may be a statement; its value is discarded (`x;`, `5;`, `foo();`). Assignment forms are the idiomatic way to get a mutation.
- **`primitive`.** A `primitive` call is an ordinary expression statement like any other (`primitive print_char('a');`), with its typing in `expressions.md#typing-rules` and its contracts in `docs/primitive.md`. Two of them, `exit` and `abort`, **end the flow of their block exactly like `return`**: nothing after them runs, and in a non-void function a path that ends in one satisfies the reaching-return check without a `return`.
- **`return`.** In a non-void function, `return expr` on every path (reaching-return check) with `expr` of exactly the return type. In a void function, `return;` or falling off the end. `return expr` in a void function is an error.
- **`if` / `else`.** The condition (`condition` in the grammar) must be `bool`. `else` binds to the nearest `if`; `else if` is spelling for a nested `if`.
- **`while` / `do` / `for`.** Conditions (`condition` in the grammar) are `bool`. `for` is C-shaped: `for (init; cond; step)` — *init* is a `let` or `const` declaration (scoped to the loop; `= expr` is optional, as with any `let`) or an assignment (a named one or a deref store, `*p = v;`), *cond* is `bool`, *step* is an assignment, including a deref store/compound (`for (i = 0; i < n; *p = *p + i)` is allowed). All three clauses are optional (a bare `for(;;)` is an infinite loop).
- **`break` / `continue`.** Legal only inside a loop body (dynamically nested in `while`/`do`/`for`); `break` in a non-loop is an error. In the example, `break` inside an `if` inside a `for` is fine.
- **Unreachable code is dropped, not rejected.** A statement that cannot execute — one after a `return` or after a terminating `primitive exit`/`abort`, or one in a loop whose body never falls through — is still parsed and fully checked, but it produces no code: the IR drops the blocks no edge can reach (see `docs/middleend/JIR.md`).

## Bindings and moves

A declaration chooses rebinding independently of object mutation:

```
const n = 7;
const mut fixed = 7;
let value = 7;
let mut mutable_value = 7;
```

`const` requires an initializer and prohibits any subsequent assignment to the binding, including compound assignment. `let` can be assigned and can omit its initializer when its type is written. `mut` records permission to mutate the bound object; it does not permit rebinding a `const`. Scalar assignment and scalar compound assignment replace the binding's value and follow the `let`/`const` rule. Access through a record without `mut` is read-only through owned fields and owned pointees. A fixed `ref mut` binding still permits changing its target; taking a mutable reference to the reference cell itself separately requires `mut` on that cell. Raw pointer operations do not gain safety or pointee immutability from a binding modifier.

A local's annotation is optional when it has an initializer. Without an annotation, an unsuffixed literal is `i32`; all other initializers keep their expression type. `void` cannot be a variable's type. A local name becomes visible after its initializer, so the initializer may read a shadowed outer variable.

`move value` consumes an initialized local variable or parameter, materializes its bits in a fresh cell, and leaves its source uninitialized. It neither empties nor mutates the source value. Reading the source again is a compile-time error until a `let` is assigned a new value. Moving a `const` is legal but assigning a new value to that binding is not. A global, literal, computed temporary, or safe field/dereference is not an operand of `move`. A raw-pointer dereference is an explicit low-level exception: `move *pointer` transfers the initialized value out of raw storage without destroying that slot. The caller must maintain that storage's initialization and ownership contract. `value = move value` is rejected. Moving a raw pointer transfers only its bits: it does not make that pointer a resource owner.

Each branch is checked from its own incoming state. Only paths reaching the join contribute to the outgoing state. A folded condition chooses its path. Loop back edges must not read a value consumed on an earlier iteration without reinitializing it. The CFG initialization analysis also checks condition, step, and `continue` paths.

Assignments have no expression value and cannot be chained. Each assignment statement has one destination and one ordinary expression on its right side.

## Structures

Fields and methods belong to the structure declaration, not a separate implementation block:

```
struct Resource {
    value: i32;
    public new(value: i32): Resource { return { value: value }; }
    public fn number(): i32 { return this.value; }
    destructor { primitive print(this.value); }
}
```

Every field, method, and factory is private by default. `public` and `private` explicitly set visibility of a member. Fields exposed as public may be modified through mutable access; protecting their invariant is the type author's responsibility. Outside access to private members is rejected. Visibility does not prevent the compiler from invoking a destructor or the copying protocol.

An ordinary instance method has an implicit `this` of type `ref Structure`. A `mut fn` has an implicit `this` of type `ref mut Structure` and requires mutable access to the receiver. `value.method(args)` borrows its receiver for the call; this does not change ordinary assignment into implicit borrowing. A `static fn` is called through the type and has no `this`. Method overloads match argument types and receiver permissions; an available mutable receiver overload is preferred when the explicit arguments also match it.

`Type.new(args)` invokes a factory. Factories may overload their parameter types, return their own structure type or `std.Result<Structure, E>`, and have no pre-existing `this`. The `{ field: expression, ... }` object literal is legal only inside that type's factory, where it assembles the value directly instead of recursively calling `new`. Every field must appear exactly once; expressions evaluate in written order before assembly. Field types provide contextual literal types. A fallible factory can pass its object literal directly to `std.Result<Structure, E>.ok(...)` or return `err(...)`. Returning either variant follows ordinary return cleanup rules. No external object-literal creation bypasses visibility or factory checks. No default factory is synthesized. A named factory can be an ordinary receiverless method delegating to `new`.

`struct Box<T> { ... }` specializes fields and methods for each concrete type argument set. `Box<i32>.new(...)` chooses a concrete structure type; function templates still deduce their arguments. A user copying method has signature `clone(): Structure`; copying requests its concrete body. Methods live in the declaring namespace and can reference the type's parameters. Owned field cycles are rejected; reference and raw-pointer recursion does not recursively own the target.

## Destruction

Destruction is automatic for every fully initialized owner. A structure may add one `destructor { ... }`, with implicit mutable `this`. First run this body, then destroy owned fields in reverse declaration order, then release the structure's storage. A structure without a user destructor still destroys its owned fields. Reference and raw-pointer fields do not own their targets.

Local owners are destroyed in reverse successful initialization order when their scope ends, including early `return`, `break`, and `continue`. A `for` initializer's owner lives through the loop and is destroyed after exiting it. By-value parameters belong to the callee and are cleaned up after body locals unless moved out. Conditional initialization and moves carry runtime live flags so cleanup destroys only values actually owned on that path. Container element destruction order belongs to the container, not the language.

Return values are copied or transferred before local cleanup. An owned temporary lasts through its full expression unless transferred into another owner; discarded temporaries are destroyed there. A reference to a temporary cannot be retained for later use. References used by a destructor must still be valid when destruction occurs.

Assignment evaluates and prepares its RHS, destroys the destination's previous owned value, then installs the new one. Self-copy prepares the copy before replacement; reference-bearing copies still undergo the usual lifetime checks. Direct self-move is rejected. Compound scalar assignments capture the original left value before later RHS calls. Traps, `primitive abort`, and immediate `primitive exit` terminate without cleanup. Ordinary return from `main` runs global cleanup after the entry function's local cleanup.

## Functions

- Parameters are `IDENT : type`, comma-separated; the return type is `[ ":" type ]` — absent means `void`.
- A function body is a single `block`. Recursion is ordinary: every call gets its own frame (see `docs/middleend/JIR.md`).
- **Declaration order is free for functions.** A function may be called before it is declared, and may call itself. Only global initializers are restricted, to globals declared earlier.
- **A function is identified by its (namespace-qualified) name plus its parameter types, and may be overloaded.** One name may carry several declarations — an **overload set** — as long as no two share the same parameter types; the return type is not part of the identity, so two declarations may not differ only in it. Declaring the same signature twice under one name is an error — `'f' is already declared`, the message a duplicate global gets too. The rule is per namespace (`#namespaces`): `a.f` and `b.f` are different functions with independent overload sets. A call picks its declaration by exact argument types, with no conversions between candidates (`expressions.md#typing-rules`).
- Arguments are passed **by value** and evaluate left to right. Reading an earlier scalar argument is completed before a later argument can change its source. Record lvalues copy through `Clonable`; record temporaries and explicit `move` transfer to the callee. References copy their reference value.
- A function sees only its own parameters/locals and the globals — never another function's locals.
- **Entry point.** The entry module must declare one `fn main(): i32` with no parameters at file scope. It runs after global initialization and its return value becomes the process exit code. A program that terminates through `primitive exit` uses that primitive's code (`docs/primitive.md`). An imported module can declare an ordinary function named `main` at file scope. A nested namespace cannot declare one.

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
- Parameter and return types may nest a type variable under pointers, references, and named structure applications: `ptr T`, `ref T`, `Box<T>`. Deduction recursively matches the pointer shape and binds its leaf; substitution reconstructs the complete parameter and return type.
- **Every type parameter must appear in a parameter type.** A parameter list that never names `T` gives deduction nothing to work from, so `fn make<T>(): T` is an error at the declaration — `type parameter 'T' of 'make' is not in the parameters`.
- **A name carries at most one template** (v1), and may carry concrete declarations beside it. On a call, a concrete declaration that matches the argument types exactly is chosen first; the template is the fallback (`expressions.md#typing-rules`). This is what lets `print<T>` sit beside an ordinary `print(char)`.
- **A template is monomorphized.** There are no templates in the IR: each distinct set of type arguments **instantiates** an ordinary function (same name, concrete parameter and return types), and calls lower to a `CALL` to it (`language.md#machine-level-contract`). An instantiation is reused for every call that deduces the same type arguments, so a template that calls itself with the same types is ordinary recursion.
- **The body is checked per instantiation**, with `T` standing for the deduced type. The bound is a promise made at the call, not a proof that the body stays inside it: an operation the bound did not name is rejected when that instantiation is compiled.
- A chain of instantiations that keeps demanding *new* types does not terminate, and is a compile-time error.

## Globals

A `global` is a top-level `let` or `const` with a required type annotation and initializer. Scalar initializers are constant expressions; record initializers may call factories and functions:

```
let counter: i64 = 0;
let multiplier: i32 = 2 + 3;
```

There is **no implicit zero**: every global must be given its starting value in the source. Backends still zero the storage before running init code (that is an implementation detail of JIR); the language contract is that a global's value is the one its initializer wrote, in declaration order, before `main` runs. `let` globals are writable at runtime (`counter = counter + 1;`); `const` globals cannot be rebound. Record globals initialize in declaration order and are destroyed in reverse order after ordinary completion of `main`. Their final storage cells are cleared. Scalar global initialization retains its constant-expression rules. Global reference rules apply independently of initialization (`types.md#references`).

## Namespaces

A `namespace NAME { … }` block (grammar: `language.md#grammar`; rules: `language.md#namespaces`) groups top-level declarations under a name, reached by qualified references (`expressions.md#names`). It holds ordinary declarations — `let`/`const` globals, structures, `fn` functions with their overload sets and templates, and nested `namespace` blocks — with the same per-name rules as the root, scoped *inside* the namespace:

- The body's declarations see each other and their enclosing namespaces by bare name; anything else is reached qualified. This is how the stdlib's functions call themselves unqualified (`docs/std.md`).
- A template declared in a namespace is a member of that namespace: its call site deduces and instantiates it there, and its body's bare names resolve in the namespace that declared it — `read_type` sees the same names (`docs/middleend/JIR.md`).
- A global in a namespace participates in the same [initialization order](language.md#modules) as file-scope globals. A qualified assignment can target it (`n.g = 5;`) when the name is visible and the binding is writable.
- A namespace containing `main` causes an entry-point error.
- One kind of thing per name, per namespace (`language.md#namespaces`): a namespace may not share its name with a global or a function it contains, and it may not be reopened. The auto-imported `stdlib/std.erx` is the root namespace `std` talking (`main.cpp`), so a program that declares `namespace std` (or any other root `std`) again gets the duplicate-declaration error.
