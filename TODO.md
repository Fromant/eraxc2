# erax TODO / roadmap

Status: **WIP.** The lexer and the parser exist; the IR is specified and implemented but never executed. The tokenizer spec is `docs/frontend/tokenizer.md`; the IR spec is `docs/middleend/JIR.md`.

## Front-end

- [x] Tokenizer (`src/frontend/tokenizer/`), pinned by `tests/Tokenizer.test.cpp`.
- [x] Parser: single pass, source → JIR, no AST (`src/frontend/parser/`), pinned by `tests/Parser.test.cpp`. Statements, scopes, definite assignment, CFG lowering, constant folding, and diagnostics are in; `--jir` prints the result.
- [x] Keyword classification (identifiers in the lexer; the parser decides).
- [ ] String/char unescaping — `char` literals are done, `string` waits on the type existing.
- [ ] `?.`/member access, `?:`, spread, ranges — the tokens compose at parse; the language side is "Beyond v1" below.

## Beyond v1 (future work)

Source-level additions that are out of scope for v1 — listed here so the language contract (`docs/core/`) stays scope-clean:

- `?:` (ternary), optional chaining (`?.`), spread (`...`), ranges, `::` namespace resolution (dots remain the namespace syntax), `const`/immutability, implicit widening.
- Short-circuit `&&` / `||` — v1 evaluates both operands (see `docs/core/expressions.md`). It needs an op the current table has none of (a conditional-jump or select form), so it is a `.jir` version bump.
- An `unreachable code` diagnostic — v1 parses dead code, checks it, and drops it.
- Strings and string literals, floats — and with them the stdlib (`std.print`). `examples/helloworld.erax` is aspirational on `string`.
- Machine-level futures — arrays/indices, pointers/references, objects/classes, closures — live in the JIR section below.

## JIR — v0.1 scope

- [x] Integer types, globals, functions, calls (incl. recursion), control flow — `src/middleend/jir/` builds and prints them; the parser emits them.
- [ ] Validator per the IR spec (`JIR.md#validator`) — the builder asserts the structural invariants in debug builds; the seven checks are not a separate pass yet.
- [ ] Interpreter: the stack-machine engine (one cell array + frame pointer).

## JIR — later scope

- Floats — they will reuse the operand's `u64` bit pattern; no format change.
- Strings — `std.string` in the stdlib, a heap-pointer wrapper; **no string ops in JIR, ever**.
- Pointers/arrays (what strings sit on).
- Closures, objects/classes.
- Externs (`std.print`, …), then imports.

## Backends

- [ ] **VM** — executes JIR from a serialized form.
- [ ] **AoT** — native codegen. Codegen concerns (instruction selection, frame layout, register allocation) are backend work.

## Serialization (`TODO.md` owns the format versioning)

`.jir` = magic + `u32` version (v0.1: 1) + deterministic little-endian encoding of `Program` (commands as op + operand list; arity from the op table). The op table is frozen by the format — a new op is a version bump.

## Conformance

Programs under `examples/` double as the conformance corpus: same files, every backend. Results are compared as **final global state + entrypoint return code**. No stdout in v0.1 — the stdlib arrives with strings.