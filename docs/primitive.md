# Primitives

A **primitive** is a function the backend provides. It is a contract: every backend — the reference interpreter, the VM, AoT — implements every primitive with exactly the behaviour this document specifies, and the behaviour is identical across them. Primitives are how a program touches the world outside the machine (output, input, process control); nothing else in the language can.

Primitives are not declared in source. The catalog below is closed: adding one is a catalog change here and in `src/primitives/`, never a language change.

## Calling

The `primitive-call` production is part of the expression grammar (`language.md#grammar`):

```
primitive-call = "primitive" IDENT "(" [ expr { "," expr } ] ")" ;
```

- `primitive` is a reserved word, and the form is an **expression**: `primitive print_char('a');` is a statement, `let c: i32 = primitive read_char();` is an initializer.
- **A separate namespace.** Only `primitive IDENT ( … )` reaches a primitive. A plain `IDENT ( … )` call is answered by function declarations only — it never resolves to a primitive — and a user function with the same name as a primitive, even the same signature, is an ordinary legal declaration. In a program that declares `fn print_char(c: char) { … }`, `print_char('a')` calls the user's function and `primitive print_char('a')` still calls the primitive.
- **Primitives are never overloaded.** The catalog holds exactly one entry per name, so `primitive IDENT` selects one signature (`print`'s variadic range below is that entry's own shape, not several entries). Overloading belongs to `fn` declarations; an overload set may freely share a spelling with a primitive — the two namespaces never mix (above).
- An unknown name is an error, and `primitive foo` without a following `(` is an error too: the keyword only ever introduces a call.
- A primitive call is **not a constant expression** (`types.md#constants`), so a global initializer cannot contain one.
- Typing follows the catalog signature: argument count and types must match exactly, and the call's type is the primitive's return type. A *lone* literal argument takes the parameter type, as everywhere — `primitive print_char(65)` and `primitive print_char('a')` both pass a `char`. The one variadic primitive is `print` (below).

## Catalog

Ids are fixed: the table order is the contract (the serialized IR names a primitive by id), and a primitive is never removed or renumbered.

| id | name        | signature                 |
| -- | ----------- | ------------------------- |
| 0  | `print`     | variadic value, `→ void`  |
| 1  | `print_char`| `(c: char) → void`        |
| 2  | `read_char` | `() → i32`                |
| 3  | `exit`      | `(code: i32) → void`      |
| 4  | `abort`     | `() → void`               |

## Contracts

### `print`

`primitive print(a, b, c);` writes one line to the program's stdout: each argument as the decimal its type reads — signed for `i*`, unsigned for `u*`, and `bool`/`char` as the byte they lower to — joined by single spaces and ended with one newline. `primitive print();` writes just the newline.

- It takes any number of value arguments, none of them `void`.
- Its bytes are part of the program's observable result.

### `print_char`

`primitive print_char(c);` appends exactly the byte `c` to stdout — no space, no newline, no formatting. Any `char` (0–255) is written as that byte. Its bytes are part of the program's observable result. It is the primitive the standard library builds all layout on (`docs/std.md`).

### `read_char`

`primitive read_char()` reads one byte from the program's stdin and returns it as an `i32` in 0–255, in order, one byte per call. At end of input it returns `-1`.

`read_char` is the only primitive whose result depends on the environment; it is deterministic relative to the bytes it is given, and a test pins it by supplying the same input.

### `exit`

`primitive exit(code);` terminates the program immediately, with `code` as the process exit code — the same thing `main`'s return value does (`statements.md#functions`). Everything written to stdout before the call **is** delivered (unlike a trap, which discards output); statements after it never run.

### `abort`

`primitive abort();` terminates the program immediately with exit code **134** (`128 + POSIX SIGABRT`, so a script can tell a panic from a graceful failure the same way on every backend), delivering everything written to stdout before it and writing no message of its own — the caller prints whatever context it needs, then aborts. Statements after it never run.

### Flow

`exit` and `abort` end the flow of the block they are in, exactly like `return`: flow analysis stops there, code after them is unreachable (parsed and checked, but dropped — `statements.md#statements`), and in a non-void function a path ending in one of them satisfies the reaching-return check. In the IR, a block ending in `PRIM exit` / `PRIM abort` has no outgoing edges, like a `RET` block (`JIR.md#primitives`).

## Determinism

Every primitive is deterministic: the same program with the same input has the same observable result on every backend. Output and termination never depend on the host beyond delivering the bytes and the code; `read_char` depends only on the input bytes it is given, which the caller/test supplies.

The parser lowers each primitive call to one `PRIM` instruction (`JIR.md#primitives`); backends execute `PRIM` straight from this catalog.
