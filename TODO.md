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

## Beyond v1 (future work)

Source-level additions that are out of scope for v1 — listed here so the language contract (`docs/core/`) stays scope-clean:

- `?:` (ternary), optional chaining (`?.`), spread (`...`), ranges, `::` namespace resolution (dots remain the namespace syntax), `const`/immutability, implicit widening.
- Short-circuit `&&` / `||` — v1 evaluates both operands (see `docs/core/expressions.md`). It needs an op the current table has none of (a conditional-jump or select form), so it is a `.jir` version bump.
- An `unreachable code` diagnostic — v1 parses dead code, checks it, and drops it.
- Strings and string literals, floats — and with them string support in the stdlib (`std.print`); v0.1 std lives in `stdlib/std.erx` already (`docs/std.md`). `examples/helloworld.erx` is aspirational on `string`.
- Machine-level futures — arrays/indices, pointers/references, objects/classes, closures — live in the JIR section below.
- **`const` / immutability.** v1 has no `const` keyword and no immutable binding: `let` declares a local or global whose initializer may or may not be a constant expression, and it stays assignable (`a = 2;`). There is no way to *say* a name must never change, which is the thing a constant expression is otherwise so careful about. See `docs/core/types.md#constants` for what "constant" currently means.
- **A `char` suffix.** `char` is a type name and `is_integer(char)` holds, but it is not a literal suffix (`types.md#literals`), so a *constant* `char` expression cannot be written: `let x: char = 255 / 16;` is an `i32` division in a `char` position. `(255 / 16) as char` already spells the value (`types.md#casts`), but only a suffix makes the arithmetic itself read as `char` arithmetic instead of `i32` ending in a cast — adding `char` to the suffix set closes that.
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
- [x] Interpreter: the stack-machine engine (one cell array + frame pointer) — `src/interp/Interpreter.{hpp,cpp}` runs a validated `Program` to completion and reports the bytes the program wrote to stdout, the final global state, and the exit code; running is the driver's default (`eraxc file.erx`, or stdin). Arithmetic is `fold::binary`/`fold::unary`, the same code the parser folds constants with, so a folded answer and a computed one cannot drift.
- [x] Primitives — the `primitive` keyword and the backend-contract catalog (`print`, `print_char`, `read_char`, `exit`, `abort`) — `docs/primitive.md`, `src/primitives/`, lowered to `PRIM` (`Op::Primitive`). They replaced the builtin `print` name-rule entirely: `print` is now an ordinary primitive call, and `fn print` is a legal declaration like any other.

## JIR — later scope

- Floats — they will reuse the operand's `u64` bit pattern; no format change.
- Strings — `std.string` in the stdlib, a heap-pointer wrapper; **no string ops in JIR, ever**.
- Pointers/arrays (what strings sit on).
- Closures, objects/classes.
- Imports — `std.erx` is auto-imported by the driver today (`docs/std.md`); a real import system (and `std.` dot access) replaces that.
- New primitives — each is a catalog addition in `docs/primitive.md`, never a language change: memory (`alloc`/`free`), raw byte output (`write_bytes`, what strings will print through), and friends. Time/random stay excluded: the determinism rule in `docs/primitive.md` is what makes a backend comparison mean anything.

## Backends

- [ ] **VM** — executes JIR from a serialized form.
- [ ] **AoT** — native codegen. Codegen concerns (instruction selection, frame layout, register allocation) are backend work.

The driver contract for both is **flag = backend** (decided): `eraxc file.erx` runs on the reference interpreter and stays the default; `--vm` / `--aot` run with that backend; `-o out.jir` / `-o out.exe` emit the artifact instead of running and imply the backend. The `Action`/backend split in `main.cpp` already leaves the slot. **The stdlib is pure erax** — `stdlib/std.erx`, embedded and auto-imported (`docs/std.md`) — so every backend runs it without implementing anything for it. What a backend *does* implement is the **primitive catalog** (`docs/primitive.md`): the interpreter, VM, and AoT all satisfy the same five contracts the same way, which is exactly what makes their results comparable.

## Serialization (`TODO.md` owns the format versioning)

`.jir` = magic + `u32` version (v0.1: 1) + deterministic little-endian encoding of `Program` (commands as op + operand list; arity from the op table; a `PRIM` carries the primitive's catalog `u32` id). The op table **and the primitive catalog** are frozen by the format — a new op or primitive is a version bump.

## Conformance

Programs under `examples/` double as the conformance corpus: same files, every backend. Results are compared as **the bytes the program wrote to stdout (`print` / `print_char`) + final global state + exit code** (the entrypoint's return, or the code `exit`/`abort` gave); a program that uses `read_char` is run with pinned input. Strings, formatting, and `std.print` still arrive later.