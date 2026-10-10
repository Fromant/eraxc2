# Primitives

A **primitive** is a function the backend provides. It is a contract: every backend — the reference interpreter, the VM, AoT — implements every primitive with exactly the behaviour this document specifies, and the behaviour is identical across them. Primitives are how a program touches the world outside the machine (output, input, process control); nothing else in the language can.

Primitives are not declared in source. The catalog below is closed: adding one is a catalog change here and in `src/primitives/`, never a language change.

## Calling

The `primitive-call` production is part of the expression grammar (`language.md#grammar`):

```
primitive-call = "primitive" IDENT "(" [ expr { "," expr } ] ")" ;
```

- `primitive` is a reserved word, and the form is an **expression**: `primitive print_char('a');` is a statement, `let c: i32 = primitive read_char();` is an initializer.
- **A separate name space.** Only `primitive IDENT ( … )` reaches a primitive. A plain `IDENT ( … )` call is answered by function declarations only — it never resolves to a primitive — and a user function with the same name as a primitive, even the same signature, is an ordinary legal declaration. In a program that declares `fn print_char(c: char) { … }`, `print_char('a')` calls the user's function and `primitive print_char('a')` still calls the primitive. Primitives are not members of any `namespace` (`language.md#namespaces`): the catalog is behind the `primitive` keyword, not behind a name.
- **Primitives are never overloaded.** The catalog holds exactly one entry per name, so `primitive IDENT` selects one signature (`print`'s variadic range below is that entry's own shape, not several entries). Overloading belongs to `fn` declarations; an overload set may freely share a spelling with a primitive — the two name spaces never mix (above).
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
| 5  | `alloc`     | `(bytes: u64) → ptr u8`   |
| 6  | `free`      | `(p: ptr u8) → void`      |

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

### `alloc`

`primitive alloc(bytes)` returns a pointer to a heap block of at least `ceil(bytes / 8)` whole cells (never fewer than one), all **zeroed** — `ptr u8`, so a store through it (`*p = v;`) names a byte-sized cell. It is the only way source gets a pointer that does not come from `&` (`expressions.md#typing-rules`). Called twice, it returns two blocks from the pool or two fresh ones — two live blocks never share cells, so a program that writes one never disturbs the other.

- The returned `ptr u8` is an address like any other: it compares equal to itself, equal to a later block only if the allocator offered the same cells again (it did not free a live block), and it can be cast to a wider pointee (`p as ptr i32`) when the program wants the block to hold one (`types.md#casts`).
- Bytes beyond the pointed cell that a wider pointee reads are whatever the block holds — the program sized the block, so staying inside it is the program's task. Reading or writing through a pointer that names no live cell is a trap, like a divide.
- An allocation that would exceed the machine's cell array is a trap, and there is no upper bound in the catalog beyond the machine's own.

### `free`

`primitive free(p)` returns the block `p` names to the allocator, so a later `alloc` may offer the same cells. `free` of the **null pointer does nothing**. Any other pointer that names no live block — an address that was never allocated, one from `&` (a stack or global cell, not a heap block), or a block already freed — is the same trap as a dead deref. After a `free`, the pointer `p` is a dead pointer: passing it to `free` again, or dereferencing it, is that trap (`JIR.md#runtime-semantics`).

### Flow

`exit` and `abort` end the flow of the block they are in, exactly like `return`: flow analysis stops there, code after them is unreachable (parsed and checked, but dropped — `statements.md#statements`), and in a non-void function a path ending in one of them satisfies the reaching-return check. In the IR, a block ending in `PRIM exit` / `PRIM abort` has no outgoing edges, like a `RET` block (`JIR.md#primitives`).

## Determinism

Every primitive is deterministic: the same program with the same input has the same observable result on every backend. Output and termination never depend on the host beyond delivering the bytes and the code; `read_char` depends only on the input bytes it is given, which the caller/test supplies.

`alloc` and `free` are deterministic in the same sense: the *block a pointer names* and its contents are fully defined, and the reference interpreter's allocator is a fixed first-fit that reuses blocks deterministically. The *address value* `alloc` returns is executor-internal, exactly as `&`'s address is (`JIR.md#runtime-semantics`) — a program compares pointers for identity and never prints or further compares an address's integer form. This is the one primitive whose value is defined only up to identity; the conformance corpus keeps to that (`TODO.md#conformance`).

The parser lowers each primitive call to one `PRIM` instruction (`JIR.md#primitives`); backends execute `PRIM` straight from this catalog.
