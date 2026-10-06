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
.\build\eraxc.exe examples\helloworld.erx   # tokenizes a file, prints a token dump
.\build\eraxc.exe --jir examples\fib.erx    # lowers a program and prints the JIR
.\build\eraxc.exe --run examples\fib.erx    # runs it; the exit code is the entrypoint's return
echo 'let a: i32 = 1;' | .\build\eraxc.exe    # or stdin; errors as file:row:col

cmake -S . -B build-cov -G Ninja -DERAX_COVERAGE=ON  # GCC/Clang + gcov only, separate dir, network first time
cmake --build build-cov --target coverage             # builds, runs the suite, writes build-cov/coverage.txt
```

`ctest` is one wrapper around the Catch2 binary; to filter you must run `build\tests\erax_tests.exe` directly. `.clang-format` (Google-based, **2-space indent**, 120 cols) governs all C++ — run `clang-format -i` on files you touch; the whole tree is already formatted. There is no lint or typecheck config. `-Wall -Wextra` apply only to GNU/Clang builds. If a coverage reconfigure fails blaming the compiler, the `build-cov/` cache is stale — delete the dir and configure again.

## Layout

- `docs/core/` — the erax language contract (v1): `language.md` (grammar, operator set, keywords, semantics) + `types.md`, `expressions.md`, `statements.md`.
- `docs/frontend/tokenizer.md` — lexical contract; `src/frontend/tokenizer/` is pinned by `tests/Tokenizer.test.cpp`.
- `src/frontend/parser/` — the implemented parser: `ProgramParser` (statements, CFG, flow/definite assignment), `ExpressionParser` (precedence, literals, calls, folding), `ParseContext` (symbols, scopes, token cursor), `SemType`, `Value`. Pinned by `tests/Parser.test.cpp`.
- `docs/middleend/JIR.md` — the shared IR contract (stack-framed, register-style); `src/middleend/jir/` implements the types, builder, constant folding, and the validator.
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
- JIR: `CALL dst &f a0…` — `args[0]`=result slot, `args[1]`=callee, `args[2..]`=args; `EMPTY` sentinel is the only legal void result slot; shift counts mask to operand width; arithmetic wraps; div-by-zero is process death (no UB anywhere).
- A loop tests in a block of its own, so the block a `while`/`for` is written in runs once and a back edge cannot re-run it.
- The interpreter reuses `fold::binary`/`fold::unary` for arithmetic, so folding and execution cannot drift; a cell is one raw `u64` bit pattern, never a host type.
- JIR prints a type with every cell operand (`i32 $3`, `u8 1`), but not with a `@k`/`&k` id, which already names a cell the program table types.
- `bool`/`char` lower to `i8`; no implicit conversions; assignment is a statement (not an expression); `break`/`continue` only in loops.
- A literal takes the type of the position it fills, and only a *lone* one: the expected type is not threaded into the operands of an operator, which is what keeps `1 < 2` a `bool` and `let x: i32 = 1 + 2` an `i32` while `let x: bool = 1;` fails.
- A folded test picks one CFG edge, so a constant `while (false)` drops its body and a constant `if` decides the definite-assignment intersection.
- Tests compare programs by final global state + entrypoint return code; no stdout in v0.1.

## Style

- C++23, no extensions. Formatting is `.clang-format` (Google-based, 2-space indent, 120 cols) — run `clang-format -i` on files you touch; match the surrounding comment style (prose comments are common).
- **No exceptions, anywhere in the front end or the driver.** Every fallible step returns `std::expected<T, E>`, and parse errors carry a `SourceLocation`; `main` reports them and returns `EXIT_FAILURE`. Do not add `throw`/`try`/`catch` or `std::optional`-then-throw patterns.
- Repo history commits each completed change directly to `main` with short imperative messages (e.g. `core: …`, `JIR: …`); no PRs.