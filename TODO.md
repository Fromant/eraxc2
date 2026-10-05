# erax TODO / roadmap

Status: **WIP.** The lexer and the parser exist; the IR is specified and implemented but never executed. The tokenizer spec is `docs/frontend/tokenizer.md`; the IR spec is `docs/middleend/JIR.md`.

## Front-end

- [x] Tokenizer (`src/frontend/tokenizer/`), pinned by `tests/Tokenizer.test.cpp`.
- [x] Parser: single pass, source → JIR, no AST (`src/frontend/parser/`), pinned by `tests/Parser.test.cpp`. Statements, scopes, definite assignment, CFG lowering, constant folding, and diagnostics are in; `--jir` prints the result.
- [x] Keyword classification (identifiers in the lexer; the parser decides).
- [x] Integer literal type suffixes — `5i64`, `0xFFu8` (`types.md#literals`). The lexer keeps the run with the digits; the parser names the type and range-checks against it.
- [x] Constant range checking — `src/frontend/parser/ConstantRange.{hpp,cpp}` holds the whole of "a constant is checked, arithmetic wraps", so removing it and the `fold` call in `ExpressionParser::apply` restores machine-level wrapping.
- [x] `char` literal unescaping — `\n`, `\t`, `\0`, `\\`, `\'`, `\xNN`, and `\uNNNN` decode in the parser.
- [ ] `string` unescaping — waits on the `string` type existing.
- [ ] `?.`/member access, `?:`, spread, ranges — the tokens compose at parse; the language side is "Beyond v1" below.

## Beyond v1 (future work)

Source-level additions that are out of scope for v1 — listed here so the language contract (`docs/core/`) stays scope-clean:

- `?:` (ternary), optional chaining (`?.`), spread (`...`), ranges, `::` namespace resolution (dots remain the namespace syntax), `const`/immutability, implicit widening.
- Short-circuit `&&` / `||` — v1 evaluates both operands (see `docs/core/expressions.md`). It needs an op the current table has none of (a conditional-jump or select form), so it is a `.jir` version bump.
- An `unreachable code` diagnostic — v1 parses dead code, checks it, and drops it.
- Strings and string literals, floats — and with them the stdlib (`std.print`). `examples/helloworld.erax` is aspirational on `string`.
- Machine-level futures — arrays/indices, pointers/references, objects/classes, closures — live in the JIR section below.
- **`const` / immutability.** v1 has no `const` keyword and no immutable binding: `let` declares a local or global whose initializer may or may not be a constant expression, and it stays assignable (`a = 2;`). There is no way to *say* a name must never change, which is the thing a constant expression is otherwise so careful about. See `docs/core/types.md#constants` for what "constant" currently means.
- **A `char` suffix.** `char` is a type name and `is_integer(char)` holds, but it is not a literal suffix (`types.md#literals`), so a *constant* `char` expression cannot be written: `let x: char = 255 / 16;` is an `i32` division in a `char` position. Adding `char` to the suffix set closes it, and is the only way `char` arithmetic is readable as arithmetic rather than through two names.
- **Literal / singleton types.** `let b: i32 = 0;` gives `b` the type `i32`, not the value `0`. A type system that tracked the value — so `b` has type `0` and a later `b = 2` is an error until the use — would catch the divide-by-a-literal-zero and the accidental-overflow class of bug at compile time instead of at the trap, and it is the natural end state of the constants work in `types.md#constants`. It costs a real type-inference term and is far past v1.
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