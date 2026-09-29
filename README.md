# erax

TypeScript-like programming language with a static type system.

One language, three ways to run it:

1. **Interpreter** — raw source, fast iteration, runs anywhere (JS-style).
2. **Bytecode VM** — the front-end compiles to a stable bytecode, JIR (`docs/middleend/JIR.md`); the VM executes it (Java/C#-style). Pre-compiled and portable, no JIT needed.
3. **AoT compiler** — native code generation for peak performance (C++-level). Coming much later; the backend itself is not part of the shared contract.

All three share one front-end (lexer → parser → JIR), so the same `.erax` programs must produce identical results on every backend. That shared contract is what makes the language cheap to test.

> **Status: WIP.** The tokenizer and the parser both exist: the parser lowers a program straight to JIR and
> `--jir` prints it. No interpreter, no VM, no AoT backend yet.

## Layout

```
CMakeLists.txt             CMake project (C++23)
src/
  frontend/
    tokenizer/             Tokenizer, Token, SourceLocation, LexError
    parser/                ProgramParser: source -> JIR, no AST (SemType, Value, ParseContext)
  middleend/jir/           Type, Op, Operand, Cfg, Builder, constant folding, printing
docs/
  core/language.md         language contract: grammar, operators, reserved words, semantics
  core/types.md            types, literals, conversions
  core/expressions.md      expressions and precedence
  core/statements.md       statements, functions, globals
  frontend/tokenizer.md    tokenizer contract
  middleend/JIR.md         the shared IR contract
TODO.md                    roadmap, serialization & format versioning
tests/
  Tokenizer.test.cpp       Catch2 test suite
  Parser.test.cpp          parser tests: accepted programs and the JIR they lower to
  Jir.test.cpp             JIR types, builder, folding, and validator
examples/                  .erax programs — the syntax vision (evolving)
main.cpp                   CLI: tokenizes a file or stdin, or lowers it to JIR
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
echo 'let a: i32 = 1;' | ./build/eraxc.exe
```

Or lower a program to IR — errors are reported as `file:row:col: error: text` either way:

```
echo 'fn main(): i32 { return 0; }' | ./build/eraxc.exe --jir
```

## Example

`examples/helloworld.erax` is the current syntax vision, and it is ahead of the implementation — `string` and
`std.print` are not v1 yet:

```erax
fn main(): i32 {
    let text: string = "Hello, \"world\"!\nOn a new line";
    std.print(text);
    return 0;
}
```

A program the parser accepts today:

```erax
fn fib(n: i32): i32 {
    if (n < 2) { return n; }
    return fib(n - 1) + fib(n - 2);
}

fn main(): i32 {
    let total: i32 = 0;
    for (let i: i32 = 0; i < 10; i = i + 1) {
        total = total + fib(i);
    }
    return total;
}
```

## Limitations / next steps

- No template literals (backticks) — they will allow raw newlines in strings.
- No `.5`-style fractions — write `0.5` (keeps `.` unambiguous for member access).
- `?.`, spread `...`, ranges have no dedicated tokens yet — the parser will compose them from `?` / `...`.
- No `::` operator — namespace resolution is dot-based (TS style). Two `:` still lex as two `:` tokens; the parser rejects them.
- No Unicode identifiers.
- Strings store raw escape text; `\"`-style unescaping happens with the parser.
- Digit separators must sit between digits — `1_000` is fine, `1_` and `0x_1` are lexer errors.
- The language is v1-incomplete on purpose: no `string`, no standard library, no float, no `?.`/member access, no
  `match`, no user types. The parser rejects them with a clear error rather than guessing.

## Design decisions

- **No AST.** The parser lowers straight to JIR in a single pass, so there is exactly one IR — `docs/middleend/JIR.md`
  — shared by interpreter, VM, and AoT backend instead of maintaining an AST *and* an IR. Likely revisited if parsing
  gets hairy.
- **All-or-nothing parsing.** A program is either fully lowered to a `Program` or rejected with one
  `file:row:col: error:` — no partial results, no exceptions anywhere in the front end.
- **Shared test corpus.** Programs under `examples/` double as the conformance suite: same files, every backend, identical observable result — final global state plus the entrypoint's return code. No stdout in v0.1; the stdlib (and `std.print`) arrives with strings.
