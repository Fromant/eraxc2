# AGENTS.md

erax, a TypeScript-like compiled language. **The tokenizer, the parser, and the JIR reference interpreter are implemented; the VM and the AoT backend are not.** The parser lowers source straight to JIR with no AST and no exceptions; the specs in `docs/` are the contract, and most sessions are still spec-writing and cross-doc consistency.

## Commands (Windows / PowerShell)

```powershell
cmake -S . -B build          # FetchContent pulls Catch2 v3.9.1 (needs network first time)
cmake --build build
ctest --test-dir build --output-on-failure
.\build\tests\erax_tests.exe                     # full suite; --list-tests enumerates the names
.\build\tests\erax_tests.exe "*loop*"            # filter by name — names are prose sentences, no tags
.\build\tests\erax_tests.exe "operator chars merge greedily"
.\build\eraxc.exe --tokenize examples\fib.erx    # prints the token list
.\build\eraxc.exe --jir examples\fib.erx          # lowers a program and prints the JIR
.\build\eraxc.exe examples\fib.erx                # runs it (no flag); the exit code is the entrypoint's return
echo 'let a: i32 = 1;' | .\build\eraxc.exe    # or stdin; errors as file:row:col

cmake -S . -B build-cov -G Ninja -DERAX_COVERAGE=ON  # GCC/Clang + gcov only, separate dir, network first time
cmake --build build-cov --target coverage             # builds, runs the suite, writes build-cov/coverage.txt
```

`ctest` is one wrapper around the Catch2 binary; to filter you must run `build\tests\erax_tests.exe` directly. `.clang-format` (Google-based, **2-space indent**, 120 cols) governs all C++ — run `clang-format -i` on files you touch; the whole tree is already formatted. There is no lint or typecheck config. `-Wall -Wextra` apply only to GNU/Clang builds. If a coverage reconfigure fails blaming the compiler, the `build-cov/` cache is stale — delete the dir and configure again.

## Layout

- `docs/core/` — the erax language contract (v1): `language.md` (grammar, operator set, keywords, semantics) + `types.md`, `expressions.md`, `statements.md`.
- `docs/frontend/tokenizer.md` — lexical contract; `src/frontend/tokenizer/` is pinned by `tests/Tokenizer.test.cpp`.
- `src/frontend/parser/` — the implemented parser: `ProgramParser` (statements, CFG, flow/definite assignment), `ExpressionParser` (precedence, literals, casts, calls, folding), `ParseContext` (symbols, scopes, token cursor), `SemType`, `Value`. Pinned by `tests/Parser.test.cpp`.
- `docs/middleend/JIR.md` — the shared IR contract (stack-framed, register-style); `src/middleend/jir/` implements the types, builder, constant folding, and the validator.
- `docs/primitive.md` + `docs/std.md` — the closed primitive catalog (`primitive` keyword, `PRIM` in IR) and the std contract; `src/primitives/Primitives.{hpp,cpp}` is the shared table the frontend, validator, and interpreter all read.
- `stdlib/std.erx` — the standard library, pure erax, embedded in the driver at configure time and auto-imported before every program (`--tokenize` excepted). Pinned by `tests/Std.test.cpp`.
- `src/interp/Interpreter.{hpp,cpp}` — the reference executor: one cell array, a frame pointer walking it, `fold` arithmetic, and the machine's own limits. Pinned by `tests/Interpreter.test.cpp`, which also runs `examples/` as the conformance corpus. Its results are what a backend is compared to.
- `TODO.md` — **roadmap + `.jir` serialization/versioning, and all "not in v1" / future-work content.** Future work must live here, never in the spec docs.
- `examples/helloworld.erx` — aspirational (uses `string`/`std.print`, which are not v1).

## Doc conventions (hard rules from the session)

- Language specs are scope-clean: anything "beyond v1" goes in `TODO.md`, not in a spec doc.
- The four `docs/core/` files cross-link (`language.md#grammar`, etc.); keep anchors, terminology, and rule statements consistent across `core/`, `tokenizer.md`, and `JIR.md` whenever editing a rule. Grep the docs after changes for leftover stale names (e.g. past renames: `VOID` sentinel → `EMPTY`, `Not in v1` → TODO).
- Docs describe a contract that the parser must enforce; type constraints live in prose (e.g. the grammar's `condition` nonterminal is shape-only; "must be `bool`" is a typing rule in `expressions.md`). The code implements the docs — when they disagree, the docs are usually the stale side, but say so before flipping either.

## Language/IR facts to not re-litigate

- Keywords are **`Identifier` tokens** classified by the parser — do not add token kinds or a keyword table to the tokenizer.
- Operators are maximal-munch runs validated against a whitelist at parse (`a+++++b` errors). Dots are never operator chars, so `?.`/`...` compose at parse; `::` lexes as two Colons and is banned (dot namespaces).
- `as` is the one conversion: `cast = unary { "as" type }` binds between binary and unary, and its operand is parsed *without* the position's expected type — that is what makes `12888 as i8` be 88 rather than a range error. `x as bool` is `x != 0`; pairs that lower alike (`u8`↔`char`, `bool`↔`i8`) retype with no instruction; everything else wraps through `fold::convert` (constants included).
- Overloads resolve by exact parameter types — no conversions between candidates, and the return type is not part of the identity. The arguments of an overloaded call are typed *before* any candidate is picked (a lone literal is `i32` unless suffixed); a single declaration is not a set and keeps the old per-parameter messages (`argument 1 of 'f' is i32, not bool`).
- JIR: `CALL dst &f a0…` — `args[0]`=result slot, `args[1]`=callee, `args[2..]`=args; `EMPTY` sentinel is the only legal void result slot; shift counts mask to operand width; arithmetic wraps; div-by-zero is process death (no UB anywhere); `CAST dst src` converts any type to any type (operand types need not match) and never fails.
- A loop tests in a block of its own, so the block a `while`/`for` is written in runs once and a back edge cannot re-run it.
- The interpreter reuses `fold::binary`/`fold::unary` for arithmetic, so folding and execution cannot drift; a cell is one raw `u64` bit pattern, never a host type.
- JIR prints a type with every cell operand (`i32 $3`, `u8 1`), but not with a `@k`/`&k` id, which already names a cell the program table types.
- `bool`/`char` lower to `i8`; no implicit conversions (casts are explicit, above); assignment is a statement (not an expression); `break`/`continue` only in loops.
- A literal takes the type of the position it fills, and only a *lone* one: the expected type is not threaded into the operands of an operator, which is what keeps `1 < 2` a `bool` and `let x: i32 = 1 + 2` an `i32` while `let x: bool = 1;` fails. The two places the position is dropped on purpose: a cast's operand, and the arguments of an overloaded call — each defaults a lone literal to `i32`.
- Templates: `fn name<T, U extends Ordered & Negatable>(…)` — deduction only (no explicit `f<u8>(x)`; `::` banned); one template per name, concrete declarations beside it, and an exact concrete match is chosen before the template is tried. Bounds are a closed vocabulary (`Ordered`/`Negatable`/`Arithmetic`/`Clonable`/`Movable`), checked against the *deduced* type at the call site, and every type parameter must appear in a parameter type. Monomorphized: each distinct type set is one ordinary function with an id reserved from the shared function-id allocator; concrete functions, record methods/helpers and template instances fill `program_.functions[id]`, so no append-order assumption is valid; `read_type` (via `ParseContext::type_substitution_`) makes `let x: T` / `as T` work per instantiation; the body is checked once per instantiation against the concrete type, never against the bound.
- Namespaces: `namespace NAME { … }` blocks nest and hold `global`/`function`/`namespace` declarations. Names are reached **qualified** from outside; the first segment searches outward from the current namespace, each later one is a direct member — so a bare `print(1)` at the root never resolves std's `print` (that is `std.print(1)`; the auto-imported stdlib is the closed root `namespace std`). Locals are never qualified, and a qualified name never names a local. One kind of thing per name per namespace; a namespace cannot be reopened; `main` only at the root. `declarations_in` and `items` recurse namespaces in identical source order, which keeps pre-pass ids aligned with the item pass (the reserved-function-id lookup in `function()`) while all instantiated methods/templates/helpers reserve ids through `function_count`. JIR stays nameless; namespace plumbing is `ParseContext` (`namespace_stack_`/`root_namespace_`, `set_namespace_path` save/restore, `TemplateInfo.ns_path`) plus token walking in `TokenCursor::past_operand`. Pin-quote the errors: `unknown namespace 'a'`, `call to undeclared function 'a.b'`, `'n.f' is a function and can only be called`, `'n' is a namespace, not a value`, `'std' is already declared`.
- Local bindings support `let`/`const`, independent `mut`, optional local type annotations, and explicit `move` of locals/parameters. `const` requires initialization and cannot be rebound, even after a move. `let` can be reinitialized. Assignment is statement-only and assignment chains (`a = b = c`) are forbidden. `Clonable` and `Movable` are independent bounds; current scalars/pointers satisfy both. Template signature types recursively represent `ptr T` and substitute/deduce through nested pointers. `check_local_flow` checks source initialization events over the CFG before pruning, including loop back edges; do not replace it with only parser-time snapshots. Structures, safe references, factories and RAII are implemented. `ProgramParser.records.cpp` owns discovery, specialization and helper lowering; `ProgramParser.effects.cpp` infers free-function invalidation; `ParseContext.ownership.cpp` owns copies, loans and cleanup. Record values lower to one heap handle, fields to cells, and `OFFSET` supplies portable cell addressing. `destructor {}` runs before automatic reverse field destruction. `this` is explicit on methods. All members default private. The only object literal syntax is factory-local `{ field: value }`. Raw `move *ptr` is a manual-storage transfer; safe partial field moves are rejected. Reference aliases are permitted; lifetime checks use CFG loan events and invalidation effects. Global scalar and raw-pointer cells can be borrowed. Global record borrows and reference-bearing globals are restricted pending interprocedural summaries. Factories may return their record or std.Result of that record. `std.unique<T>` / `std.vector<T>` are pure erax.
- A folded test picks one CFG edge, so a constant `while (false)` drops its body and a constant `if` decides the definite-assignment intersection.
- Tests compare programs by the bytes `PRIM print`/`print_char` wrote, final global state, and exit code (the entrypoint's return, or the code `exit`/`abort` gave); the primitives are the only stdout in v0.1, and a `terminating` `PRIM` closes its block exactly like `RET`.

## Style

- C++23, no extensions. Formatting is `.clang-format` (Google-based, 2-space indent, 120 cols) — run `clang-format -i` on files you touch; match the surrounding comment style (prose comments are common).
- **No exceptions, anywhere in the front end or the driver.** Every fallible step returns `std::expected<T, E>`, and parse errors carry a `SourceLocation`; `main` reports them and returns `EXIT_FAILURE`. Do not add `throw`/`try`/`catch` or `std::optional`-then-throw patterns.
- Repo history commits each completed change directly to `main` with short imperative messages (e.g. `core: …`, `JIR: …`); no PRs.