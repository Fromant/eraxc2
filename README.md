# erax

TypeScript-like programming language with a static type system.

One language, three ways to run it:

1. **Reference interpreter** — runs JIR in this process, one cell array at a time. This is what `eraxc` does by default, and its results are the ones every other backend is compared against.
2. **Bytecode VM** — the front-end compiles to a stable bytecode, JIR (`docs/middleend/JIR.md`); the VM executes a *serialized* form of it (Java/C#-style). Pre-compiled and portable, no JIT needed.
3. **AoT compiler** — native code generation for peak performance (C++-level). Coming much later; the backend itself is not part of the shared contract.

All three share one front-end (lexer → parser → JIR), so the same `.erx` programs must produce identical results on every backend. That shared contract is what makes the language cheap to test.

> **Status: WIP.** The tokenizer, the parser, and the reference interpreter all exist: the parser lowers a
> program straight to JIR, `--jir` prints it, and running it is the default (no flag needed). The validator
> runs on everything the driver lowers. No bytecode VM, no AoT backend yet.

## Using eraxc

`eraxc` is the driver. It reads one `.erx` file, or stdin when no file is given, and runs the full pipeline by
default. A flag only ever stops the pipeline earlier:

| Command | What it does |
| ------- | ------------ |
| `.\build\eraxc.exe file.erx` | Parse, validate, and run: the program's stdout bytes and final globals on stdout, **exit code = the entrypoint's `i32` return** (or `exit`/`abort`'s code) |
| `.\build\eraxc.exe --run file.erx` | The no-flag behavior, written out |
| `.\build\eraxc.exe --jir file.erx` | Stop after lowering: print the validated JIR — what a backend executes |
| `.\build\eraxc.exe --tokenize file.erx` | Stop after the lexer: print the token list |
| `.\build\eraxc.exe --help` | Usage |

Every failure exits `1`. A source error quotes its location (`file:row:col:`), a rejected IR or a trap quotes
the place in the IR instead, because that is all the IR has to go on.

Start with the tokenizer — no build step, just what the source lexes as:

```powershell
.\build\eraxc.exe --tokenize examples\fib.erx
```

```
identifier 'fn' examples\fib.erx:13:1
identifier 'fib' examples\fib.erx:13:4
'(' '(' examples\fib.erx:13:7
identifier 'n' examples\fib.erx:13:8
':' ':' examples\fib.erx:13:9
identifier 'i32' examples\fib.erx:13:11
')' ')' examples\fib.erx:13:14
...
end of file '' examples\fib.erx:22:2
```

Every token carries the kind, the text, and where it was — that dump is what you read when the lexer is
the thing in question, and the location is what every later error is quoted against.

Then lower it. This is what a backend is handed, and what the validator checked first:

```powershell
.\build\eraxc.exe --jir examples\sum_to_ten.erx
```

```
function 0: () -> i32, frame 5
  0:
    MOV i32 $0 i32 0
    MOV i32 $1 i32 0
    -> 1
  1:
    CMP_LT i8 $2 i32 $1 i32 10
    if CMP_NE i8 $2, i8 0 then 3
    -> 2
  2:
    RET i32 $0
  3:
    ADD i32 $4 i32 $0 i32 $1
    MOV i32 $0 i32 $4
    -> 4
  4:
    ADD i32 $3 i32 $1 i32 1
    MOV i32 $1 i32 $3
    -> 1
entrypoint: 0
```

Blocks are numbered, `-> n` is the fall-through edge, and block 1 is the loop's own header: block 4 jumps
back to it, so nothing before it re-runs.

Then run it. No flag needed — running is the default, and the exit code is the program's own result, so it composes with everything else in a shell:

```powershell
.\build\eraxc.exe examples\fib.erx       # prints nothing, exits 55
.\build\eraxc.exe examples\globals.erx
global 0: i32 0x2                        # the final global state
# exits 2
```

A program with no globals and no output writes nothing of its own — globals plus exit code is still the whole result unless it calls `primitive print` / `primitive print_char` (or a std function that does), which are the only ways a v0.1 program writes to stdout. Stdin works everywhere a file does, which is the fastest way to try a snippet:

```powershell
'fn main(): i32 { return 7; }' | .\build\eraxc.exe   # exits 7
```

Errors stop it wherever they are — in the source, in the IR, or at run time:

```powershell
'fn main(): i32 { let b: bool = 1; return 0; }' | .\build\eraxc.exe
<stdin>:1:22: error: expected a value of type bool, not i32

'fn main(): i32 { let z: i32 = 0; return 7 / z; }' | .\build\eraxc.exe
error: function 0, block 0, 1 frame deep: division by zero
```

## Layout

```
CMakeLists.txt             CMake project (C++23)
src/
  frontend/
    tokenizer/             Tokenizer, Token, SourceLocation, LexError
    parser/                ProgramParser: source -> JIR, no AST (SemType, Value, ParseContext)
  middleend/jir/           Type, Op, Operand, Cfg, Builder, constant folding, printing, Validator
  primitives/              Primitives.{hpp,cpp}: the shared primitive catalog (print, print_char, read_char, exit, abort)
  interp/                  Interpreter: the reference executor (one cell array + frame pointer)
docs/
  core/language.md         language contract: grammar, operators, reserved words, semantics
  core/types.md            types, literals, conversions
  core/expressions.md      expressions and precedence
  core/statements.md       statements, functions, globals
  frontend/tokenizer.md    tokenizer contract
  middleend/JIR.md         the shared IR contract
  primitive.md             the `primitive` keyword and the closed primitive catalog
  std.md                   the standard library contract
TODO.md                    roadmap, serialization & format versioning
stdlib/
  std.erx                  the standard library (pure erax, auto-imported as `namespace std` before every program)
  std.test.erx             the std self-test: plain checks and a tally, returns the failure count
tests/
  Tokenizer.test.cpp       lexer tests
  ParserTest.hpp           shared parser-test helpers (parse_ok / parse_error / ops_of)
  Parser.test.cpp          parser headline cases, accepted programs and their JIR
  Parser.literals.test.cpp literals, constant folding, and type ranges
  Records.test.cpp         factories, visibility, copying, destructors, and global owners
  References.test.cpp      inferred reference dependencies and safe access
  Collections.test.cpp     pure-erax unique/vector conformance and storage reuse
  Ownership.test.cpp       bindings, moves, assignment chains, and heap lifetime regressions
  Parser.globals.test.cpp  globals, declarations, control flow, and CFG lowering
  Parser.errors.test.cpp   rejection messages and error locations
  Jir.test.cpp             JIR types, builder, and folding
  Validator.test.cpp       the seven validator checks, one rule broken at a time
  Primitives.test.cpp      the primitive catalog as the frontend, validator, and interpreter share it
  Std.test.cpp             std.erx functions: the std namespace, output templates, char and math helpers
  Parser.namespaces.test.cpp namespace blocks, qualified names, and namespace errors
  Interpreter.test.cpp     what programs do when they run, plus the examples/ corpus
examples/                  .erx programs — the syntax vision and the conformance corpus
main.cpp                   CLI: run a file or stdin by default; `--jir` / `--tokenize` stop the pipeline earlier
```

## Building

All dependencies come from CMake `FetchContent` (git + a C++23 compiler required).

```
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The first configure fetches Catch2, so it needs network once. After that, `.\build\eraxc.exe --help` lists the driver.

`ctest --test-dir build` is one entry wrapping the whole suite. The Catch2 binary lives at
`build\tests\erax_tests.exe`; run it directly to filter, since test names are prose sentences with no tags:

```powershell
.\build\tests\erax_tests.exe "*loop*"
.\build\tests\erax_tests.exe "operator chars merge greedily"
```

## Coverage

The suite is instrumented with gcov, and one command runs it and prints a per-source table
(GCC or Clang; the first configure needs network for Catch2):

```
cmake -S . -B build-cov -G Ninja -DERAX_COVERAGE=ON
cmake --build build-cov --target coverage
```

The report lands in `build-cov/coverage.txt`. `branches visited` counts a branch once either
outcome runs; `taken both ways` counts it only when both do, so that column is the one to
push toward 100%.

## Example

`examples/helloworld.erx` is the current syntax vision, and it is ahead of the implementation — `string` and
`std.print` are not v1 yet:

```erax
fn main(): i32 {
    let text: string = "Hello, \"world\"!\nOn a new line";
    std.print(text);
    return 0;
}
```

A program the parser accepts today, and the interpreter runs — this is `examples/fib.erx`, minus its
comment header:

```erax
fn fib(n: i32): i32 {
    if (n < 2) {
        return n;
    }
    return fib(n - 1) + fib(n - 2);
}

fn main(): i32 {
    return fib(10);
}
```

```powershell
.\build\eraxc.exe examples\fib.erx      # prints nothing, exits 55
```

## Limitations / next steps

- No template literals (backticks) — they will allow raw newlines in strings.
- No `.5`-style fractions — write `0.5` (keeps `.` unambiguous for member access).
- `?.`, spread `...`, ranges have no dedicated tokens yet — the parser will compose them from `?` / `...`.
- No `::` operator — namespace resolution is dot-based (TS style). Two `:` still lex as two `:` tokens; the parser rejects them.
- No Unicode identifiers.
- Strings store raw escape text; `\"`-style unescaping happens with the parser.
- Digit separators must sit between digits — `1_000` is fine, `1_` and `0x_1` are lexer errors.
- Structures support private/public fields, factories, methods, explicit references/moves, and RAII destructors.
  The pure-erax standard library provides `std.unique<T>` and `std.vector<T>`; see `examples/raii.erx`.
  Strings, floats, optional chaining, and pattern matching remain outside the implemented language.
- Two ways to print: `primitive print(a, b, …)` writes each value as the decimal its type reads, space-separated,
  ending in one newline; `primitive print_char(c)` writes one byte. The standard library (`stdlib/std.erx`,
  auto-imported before every program) lives in the root namespace `std` and is reached qualified
  (`docs/core/language.md#namespaces`) — `std.print` is a template plus overloads covering `i8` … `u64`, `bool`,
  `char`, and a zero-argument `std.print()` that writes one newline, each just the value with no spacing; compose
  separators with `std.print(' ')` / `std.print('\n')`. No strings or formatting yet — those arrive with `string`.
- The interpreter bounds its own recursion and cell array (`Limits`, in `src/interp/Interpreter.hpp`) and reports
  `StackExhausted` rather than growing until the host gives out.

## Design decisions

- **No AST.** The parser lowers straight to JIR in a single pass, so there is exactly one IR — `docs/middleend/JIR.md`
  — shared by interpreter, VM, and AoT backend instead of maintaining an AST *and* an IR. Likely revisited if parsing
  gets hairy.
- **All-or-nothing parsing.** A program is either fully lowered to a `Program` or rejected with one
  `file:row:col: error:` — no partial results, no exceptions anywhere in the front end.
- **Validate before running.** Every operand is read through an index, so the machine asks the validator first: a
  program it rejects never reaches the cell array.
- **One folder, one answer.** The interpreter's arithmetic is `fold::binary`/`fold::unary`, the same code the parser
  folds constants with, so a folded answer and a computed one cannot drift apart.
- **A cell is one `u64`.** A value is never held in a host type, so wrapping and narrowing are the IR's business:
  an `i32` cell is zero-extended, and `i32 -1` is `0xFFFFFFFF`.
- **Shared test corpus.** Programs under `examples/` double as the conformance suite: same files, every backend, identical observable result — the stdout bytes (`primitive print` / `print_char`, directly or through std), final global state, and the exit code (the entrypoint's return, or the code `exit`/`abort` gave). The primitives are the only stdout in v0.1; strings and `std.print` arrive later.
