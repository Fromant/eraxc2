# erax TODO / roadmap

Status: **WIP.** The lexer and the parser exist; the IR is specified and implemented, validated, and executed by the reference interpreter. The tokenizer spec is `docs/frontend/tokenizer.md`; the IR spec is `docs/middleend/JIR.md`.

## Front-end

- [x] Tokenizer (`src/frontend/tokenizer/`), pinned by `tests/Tokenizer.test.cpp`.
- [x] Parser: single pass, source → JIR, no AST (`src/frontend/parser/`), pinned by `tests/Parser.test.cpp`. Statements, scopes, definite assignment, CFG lowering, constant folding, and diagnostics are in; `--jir` prints the result.
- [x] Keyword classification (identifiers in the lexer; the parser decides).
- [x] Integer literal type suffixes — `5i64`, `0xFFu8` (`types.md#literals`). The lexer keeps the run with the digits; the parser names the type and range-checks against it.
- [x] Constant range checking — `src/frontend/parser/ConstantRange.{hpp,cpp}` holds the whole of "a constant is checked, arithmetic wraps", so removing it and the `fold` call in `ExpressionParser::apply` restores machine-level wrapping.
- [x] `char` literal unescaping — `\n`, `\t`, `\0`, `\\`, `\'`, `\xNN`, and `\uNNNN` decode in the parser.
- [ ] `string` unescaping — waits on the `string` type existing.
- [ ] `?.`/member access, `?:`, spread, ranges — the tokens compose at parse; the language side is "Beyond v1" below.
- [x] `primitive` keyword + `primitive-call` expression (`language.md#grammar`), the catalog, and std auto-import (`docs/std.md`).
- [x] `as` cast — `cast = unary { "as" type }` between binary and unary (`expressions.md#precedence`), any type to any type, always wrapping via `fold::convert` (`types.md#casts`, `JIR.md#conversions`), lowered to `CAST`.
- [x] Function overloading — name plus parameter types identify a declaration (`statements.md#functions`), calls resolve by exact argument types (`expressions.md#typing-rules`), and std's `print` is one name with a declaration per type (`docs/std.md`).
- [x] Templates — `fn name<T extends Ordered & Negatable>(…)` with type arguments deduced from the call, capability bounds checked at the call site, and one monomorphized function per distinct instantiation (`docs/core/statements.md#templates`, `docs/core/types.md#capabilities`, `docs/core/expressions.md#typing-rules`).
- [x] Namespaces — `namespace NAME { … }` blocks (nestable), dot-qualified references (`std.print(x)`), the closed `namespace std` as the auto-imported stdlib, and per-namespace name/clash rules. JIR stays nameless; templates instantiate inside the namespace that declared them (`docs/core/language.md#namespaces`, `docs/core/statements.md#namespaces`).

## Ownership and RAII

Implemented contract lives in `docs/core/`, `docs/std.md`, and `docs/middleend/JIR.md`. VM/AoT remain deferred. `examples/raii.erx` and the record/reference/collection tests pin the target `std.vector<std.unique<Resource>>`, relocation, return-by-move, and exactly-once destruction.

- [x] Heap liveness and zeroed reuse, portable cell offsets, and bounded-memory reuse tests.
- [x] Independent `let`/`const` and `mut`, local inference, explicit whole-value moves, and CFG back-edge initialization checks.
- [x] Assignment statement-only; **all chains `a = b = c` are forbidden** (revised decision).
- [x] Named generic structures, recursive type applications, fields, factories, methods, private/public access, and owned-cycle validation.
- [x] Safe `ref` / `ref mut`, reference fields, aliases, local lifetime dependencies, and invalidation checks through control flow.
- [x] Automatic field/local/global cleanup, temporaries and parameters, user `destructor {}`, and `clone`-based copying independent of movement.
- [x] Pure-erax `unique<T>` and `vector<T>` with relocation, cloning, checked access, clear/pop/release, and user element cleanup.

Remaining work and deliberate restrictions:

- **Fallible factories / Result / Option.** Factories can return the library type `std.Result<T, E>`. Dedicated propagation syntax and `Option` remain open. This is separate from traps/abort/exit, which deliberately do not unwind.
- **Global reference summaries.** Global scalar and raw-pointer cells can be borrowed. Safe references to global record owners or their fields and globals containing safe references remain restricted. Sound support needs interprocedural escape/invalidation and teardown-dependency summaries, not just permanent global-cell addresses. Global owning records already initialize in source order and destroy after `main` in reverse order. Destructor access to an already destroyed global owner traps; global initialization/teardown dependencies need an explicit dependency policy.
- **More precise inferred lifetimes.** Current dependency sets combine possible sources conservatively. Replacing/removing a reference can retain obsolete dependencies. Copying reference-bearing records retains source lifetime dependencies because a custom clone may borrow the source's owned fields. Additional mutable record-reference method/factory arguments and unknown alias/generic effects are conservative invalidation boundaries. Refine per-field, container, clone-result and call summaries before considering user lifetime annotations.
- **Temporary reference receivers.** Reference-returning calls on unnamed owned temporaries are rejected even for immediate uses that could be safe within one full expression. Add scoped temporary loans if needed.
- **Movability control and self references.** All current structures infer movement from fields. Explicit opt-out/non-relocatable storage, pinning, self-referential construction and partial safe field moves need their own contract; no user move method is defined. Raw pointer storage remains manual and unchecked at the language level.
- **Generic methods and type operations.** Generic record types and generic free functions exist; methods cannot declare additional type parameters. Add method-level generic bounds only with explicit resolution rules. Layout is cell based and record storage is boxed; inline aggregates/packed bytes, a source `sizeof`, slices, indexing protocols, iterator protocols, Unicode-aware string operations and richer collections need separate design. Do not encode address-region bits in stdlib.
- **Front-end architecture.** Storage places and ownership/loan metadata are explicit in `Value` and `ParseContext`; CFG events check initialization and loans. Further growth may warrant a typed operations layer before JIR lowering. Avoid duplicating cleanup logic or introducing backend-specific destructor instructions.

## Beyond v1 (future work)

Source-level additions that are out of scope for v1 — listed here so the language contract (`docs/core/`) stays scope-clean:

- `?:` (ternary), optional chaining (`?.`), spread (`...`), ranges, `::` namespace resolution (dots remain the namespace syntax), implicit widening.
- Short-circuit `&&` / `||` — v1 evaluates both operands (see `docs/core/expressions.md`). It needs an op the current table has none of (a conditional-jump or select form), so it is a `.jir` version bump.
- An `unreachable code` diagnostic — v1 parses dead code, checks it, and drops it.
- Floating-point types and formatted output.
- Machine-level futures — arrays/indices, closures, and inline aggregate layout — live in the JIR section below. Structures and safe references are implemented. Raw pointers (`ptr T`, `&`, `*`) and the `alloc`/`free` heap are implemented.
- **A `char` suffix.** `char` is a type name and `is_integer(char)` holds, but it is not a literal suffix (`types.md#literals`), so a *constant* `char` expression cannot be written: `let x: char = 255 / 16;` is an `i32` division in a `char` position. `(255 / 16) as char` already spells the value (`types.md#casts`), but only a suffix makes the arithmetic itself read as `char` arithmetic instead of `i32` ending in a cast — adding `char` to the suffix set closes that.
- **Literal / singleton types.** `let b: i32 = 0;` gives `b` the type `i32`, not the value `0`. A type system that tracked the value — so `b` has type `0` and a later `b = 2` is an error until the use — would catch the divide-by-a-literal-zero and the accidental-overflow class of bug at compile time instead of at the trap, and it is the natural end state of the constants work in `types.md#constants`. It costs a real type-inference term and is far past v1.
- **More template machinery.** v1 templates (`docs/core/statements.md#templates`) are deliberately minimal: **one template per name**, type arguments **always deduced**, and a **closed** capability bound vocabulary (`docs/core/types.md#capabilities`). Each extension has a cost. A second template per name needs a rule for which one a call picks — v1 says the name decides, and a concrete declaration that matches exactly wins. **Explicit instantiation** (`f<u8>(x)`) reopens the ambiguity that kept it out: `a < b > (c)` is an unknown-operator error today (`docs/core/language.md#operator-set`), and making it a call needs either a `::`-style token (banned — dots are the namespace syntax) or a grammar rule the language does not have. A **wider capability vocabulary** veers into a full interface system — `Equatable`, `Bitwise`, ordering beyond `<`, and user-declarable bounds and the types that name them — which is real type-system work well past v1.
- **Definition-time template body checking.** v1 type-checks a template's body **once per instantiation**, against the deduced concrete type. A better design checks it **once, against the bound**, so a body that uses an operator the bound does not promise is an error at the declaration, not at the first call. That needs the checker to reason about "some `T` satisfying these capabilities" — a type variable in the type system — which the concrete-per-instantiation pass sidesteps.
- **Wider integers (`i128`/`i256`) and arbitrary precision.** The question that decides whether it is cheap or not: a *cell* is one 64-bit word in the IR (`JIR.md#types`), so a value wider than a machine word is not one cell, and every assumption below is keyed to a single word. In order of how much it costs:
  1. `Operand::value`, `ConstInt::bits`, and the `EMPTY` sentinel are one `uint64_t` — a value of `2^64` is exactly the case `types.md#constants` classifies as *in no type*.
  2. `fold::narrow` masks with a width taken from the type, and `size_of` returns a small count; both assume a value fits a word, and `i128` is the first type that breaks it.
  3. A frame is a fixed number of cells and `frame_size` is a compile-time constant per function (`JIR.md#frames`), so a 2-cell value has to be laid out and copied by every frame operation, not just by the arithmetic.
  4. The `u64` evaluation range in `types.md#constants` is the front end's exact-arithmetic domain — the widest range a literal suffix can name. Widening it is a real change there, and is where the `2^64` bug above came from.
  5. Shift masking, `to_signed`/`reinterpret`, printing, and `.jir` serialization all carry the word assumption.

  Adding the *types* to the enums is easy; making them mean something is not. The clean version is to give the front end a value type that is independent of the widest machine type (so `ConstInt` stops being a `uint64_t` in a struct) before adding any type wider than a word.

## JIR — v0.1 scope

- [x] Integer types, globals, functions, calls (incl. recursion), control flow — `src/middleend/jir/` builds and prints them; the parser emits them.
- [x] Validator per the IR spec (`JIR.md#validator`) — `src/middleend/jir/Validator.{hpp,cpp}` runs the seven checks on every `Program` the driver lowers; it collects all errors rather than stopping at the first, so one run names everything wrong.
- [x] Interpreter: the stack-machine engine (one cell array + frame pointer) — `src/interp/Interpreter.{hpp,cpp}` runs a validated `Program` to completion and reports the bytes the program wrote to stdout, the final global state, and the exit code; running is the driver's default (`eraxc file.erx`, or stdin). Arithmetic is `fold::binary`/`fold::unary`, the same code the parser folds constants with, so a folded answer and a computed one cannot drift.
- [x] Primitives — the `primitive` keyword and the backend-contract catalog (`print`, `print_char`, `read_char`, `exit`, `abort`) — `docs/primitive.md`, `src/primitives/`, lowered to `PRIM` (`Op::Primitive`). They replaced the builtin `print` name-rule entirely: `print` is now an ordinary primitive call, and `fn print` is a legal declaration like any other.

## JIR — later scope

- Floats — they will reuse the operand's `u64` bit pattern; no format change.
- Strings — `std.string` in the stdlib, a heap-pointer wrapper; **no string ops in JIR, ever**. It sits on the raw pointers below.
- Ownership refinements, arrays/indices, slices and iterator protocols — see [Ownership and RAII](#ownership-and-raii). Raw pointers remain available underneath the implemented managed types.
- Closures, objects/classes.
- Modules and packages: see [Module system](#module-system).
- New primitives — each is a catalog addition in `docs/primitive.md`, never a language change: memory (`alloc`/`free`) landed; raw byte output (`write_bytes`, what strings will print through), and friends. Time/random stay excluded: the determinism rule in `docs/primitive.md` is what makes a backend comparison mean anything.

## Module system

Use ES-style named imports with optional aliases and explicit exports.

```ts
import { parse, ParseError as ConfigError } from "./config.erx";
import { net } from "./network.erx";
```

- Each file has its own root scope. Imports precede declarations and are visible in nested namespaces under the existing lookup rules. Duplicate root bindings report both locations. Use named bindings throughout.
- Declarations are private by default. Use `export` for functions, structures, globals, namespaces and their public members. Structure member access follows existing rules. Export a function's overloads and optional template as one set. Imported templates resolve names in their declaring module.
- Resolve relative paths from the importing file and require the exact filename including `.erx`. Use resolved file identity to share types and globals across importers. Reject cycles with the import chain. Initialize dependencies before importers, visit sibling imports in source order and preserve global declaration order within each file. Use the entry file's root `main`.
- Resolve imports in the frontend and bind them to existing declarations using the shared JIR ID allocators. Aliases preserve declaration identity.

Implementation order

1. Implement relative imports and exports. Verify visibility, aliases, overloads, template resolution, shared identity in diamond dependencies, global initialization order and diagnostics.
2. Route embedded std through the module loader while preserving automatic `std.print(...)` access. Resolve the compiler's `std.string` and `std.Result` hooks by declaration identity. Decide separately whether to require an explicit std import later.
3. Add a manifest for local package dependencies and public entry points such as `"acme.http"`. Add named re-exports such as `export { Request, connect } from "./client.erx";`.
4. Add version resolution, a lockfile and registry support. The package manager pins dependencies and the compiler consumes the resolved graph.

## Backends

- [ ] **VM** — executes JIR from a serialized form.
- [ ] **AoT** — native codegen. Codegen concerns (instruction selection, frame layout, register allocation) are backend work.

The driver contract for both is **flag = backend** (decided): `eraxc file.erx` runs on the reference interpreter and stays the default; `--vm` / `--aot` run with that backend; `-o out.jir` / `-o out.exe` emit the artifact instead of running and imply the backend. The `Action`/backend split in `main.cpp` already leaves the slot. **The stdlib is pure erax** — `stdlib/std.erx`, embedded and auto-imported (`docs/std.md`) — so every backend runs it without implementing anything for it. What a backend *does* implement is the **primitive catalog** (`docs/primitive.md`): the interpreter, VM, and AoT all satisfy the same five contracts the same way, which is exactly what makes their results comparable.

## Serialization (`TODO.md` owns the format versioning)

`.jir` = magic + `u32` version + deterministic little-endian encoding of `Program` (commands as op + operand list; arity from the op table; a `PRIM` carries the primitive's catalog `u32` id). The op table **and the primitive catalog** are frozen by the format — a new op or primitive is a version bump. The pointer phase added `LOAD`/`STORE`/`ADDR`, the `ptr T` type, and `alloc`/`free`; the ownership phase appended `OFFSET` and the `offset` primitive (id 7). Typed allocation appended `alloc_cells` (id 8), preserving existing ids; when serialization lands it must carry a version past the one the five-primitive, nineteen-op set was frozen at.

## Conformance

Programs under `examples/` double as the conformance corpus: same files, every backend. Results are compared as **the bytes the program wrote to stdout (`print` / `print_char`) + final global state + exit code** (the entrypoint's return, or the code `exit`/`abort` gave); a program that uses `read_char` is run with pinned input.

**Pointer caveat.** A pointer's *value* is the executor's address encoding (`docs/middleend/JIR.md#runtime-semantics`): identity (`p == q`) is conformance-safe, but a program that prints an address or otherwise lets a specific `&`/`alloc` value out of its program is not — the corpus's pointer programs compare blocks and identity only. Whether raw address values should be made deterministic (or excluded from conformable programs) is open.
