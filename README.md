# erax

TypeScript-like programming language with a static type system.

One language, three ways to run it:

1. **Interpreter** — raw source, fast iteration, runs anywhere (JS-style).
2. **Bytecode VM** — the front-end compiles to a stable bytecode, JIR (`docs/middleend/JIR.md`); the VM executes it (Java/C#-style). Pre-compiled and portable, no JIT needed.
3. **AoT compiler** — native code generation for peak performance (C++-level). Coming much later; the backend itself is not part of the shared contract.

All three share one front-end (lexer → parser → JIR), so the same `.erax` programs must produce identical results on every backend. That shared contract is what makes the language cheap to test.

> **Status: WIP.** Only the lexer exists today. No parser, no interpreter yet.

## Layout

```
CMakeLists.txt             CMake project (C++23)
src/
  frontend/
    tokenizer/             Tokenizer, Token, SourceLocation, LexError
docs/
  frontend/tokenizer.md    tokenizer contract
  middleend/JIR.md         the shared IR contract (spec; unbuilt)
TODO.md                    roadmap, serialization & format versioning
tests/
  Tokenizer.test.cpp       Catch2 test suite
examples/                  .erax programs — the syntax vision (evolving)
main.cpp                   CLI: tokenizes a file or stdin and prints the tokens
```

## Building

All dependencies come from CMake `FetchContent` (git + a C++23 compiler required).

```
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Run the tokenizer on source:

```
./build/eraxc.exe examples/helloworld.erax
echo 'let a = 1;' | ./build/eraxc.exe
```

## Example

`examples/helloworld.erax` is the current syntax vision:

```erax
fn main(): i32 {
    let text: string = "Hello, \"world\"!\nOn a new line";
    std.print(text);
    return 0;
}
```

The parser is not written yet — the CLI currently prints a token dump.

## Limitations / next steps

- No template literals (backticks) — they will allow raw newlines in strings.
- No `.5`-style fractions — write `0.5` (keeps `.` unambiguous for member access).
- `?.`, spread `...`, ranges have no dedicated tokens yet — the parser will compose them from `?` / `...`.
- No `::` operator — namespace resolution is dot-based (TS style). Two `:` still lex as two `:` tokens; the parser rejects them.
- No Unicode identifiers.
- Strings store raw escape text; `\"`-style unescaping happens with the parser.
- Digit separators must sit between digits — `1_000` is fine, `1_` and `0x_1` are lexer errors.

## Design decisions

- **No AST.** The parser is planned to lower straight to JIR (single pass), so there is exactly one IR — `docs/middleend/JIR.md` — shared by interpreter, VM, and AoT backend instead of maintaining an AST *and* an IR. Likely revisited if parsing gets hairy.
- **Shared test corpus.** Programs under `examples/` double as the conformance suite: same files, every backend, identical observable result — final global state plus the entrypoint's return code. No stdout in v0.1; the stdlib (and `std.print`) arrives with strings.
