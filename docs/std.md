# The standard library

`stdlib/std.erx` **is** the standard library: ordinary erax source, compiled with every program. The driver parses it **before** the file it was given (auto-import), so its declarations are part of every program — the root `namespace std` is already there, and a program reaches it qualified (`std.print(x)`). When the import system arrives (`TODO.md`), auto-import goes away and std becomes an ordinary imported module — nothing in this file's *contents* changes, only how it gets in.

The source of truth is `stdlib/std.erx` itself; it is embedded in the compiler at build time, so compiling never depends on files next to the source.

Alongside it, `stdlib/std.test.erx` is the **std self-test**: ordinary erax, no harness past two counters — `check(cond)` bumps a `checks` counter and a `failed` counter, `main` runs one assertion per behavior, prints the tally, and *returns* the failure count, so the exit code is 0 exactly when the suite passed. It runs anywhere the compiler runs, and the interpreter test suite lowers the checked-in copy (embedded like std itself) and pins its output and exit code.

## Rules

- **std's names are its own namespace.** std's declarations all live inside `namespace std` (`language.md#namespaces`), and the auto-import registers that namespace before the user's declarations: user code reaches std's functions only as `std.…` — there is no bare `print` — and std's names are separate from the user's, so a user function named `abs` or `print` at the root or in a namespace does not clash with std's (`statements.md#namespaces`). Inside `namespace std`, the functions call each other by bare name, exactly as the enclosing-scope rules say. `namespace std` (or another root `std`) may not be reopened by the program.
- **std is closed.** It may call primitives and other std functions, never user code: it is fully parsed before the user's declarations exist, so the reverse could not resolve anyway.
- **No globals, no `main`.** std declares no globals (so it cannot affect a program's observable global state) and never contains the entrypoint.
- Everything std can do, user code can do too: std has no privileges — it uses the same primitives, the same language, and the same templates.

## Contents (v0.1)

Each name below is reached qualified — `std.print`, `std.min`, `std.assert` — and spelled bare here only where it is named as a function.

- **Output** — the number or the word only, no spacing around it: `print` is the template `fn print<T extends Ordered & Arithmetic>(value: T)`, which writes every integer's decimal by recursive division (`value / 10` then one digit per `print_char`). A signed value prints its sign, then its magnitude, read as a wrapping `u64` — `0u64 - (value as u64)` is `i64min`'s magnitude too, so nothing is written out specially. `print(bool)` writes the text `true` / `false`, `print(char)` writes the byte itself, and the zero-argument `print()` writes one newline. Composition — separators, line breaks, columns — is the caller's, written with `std.print(' ')` / `std.print('\n')` or `primitive print_char`. (`primitive print` remains the ready-made "values on one line" case.)
- **Char** — `is_digit`, `is_alpha`, `is_alnum`, `is_upper`, `is_lower` (`→ bool`, ASCII), `to_digit` (`→ i32`, `-1` when not a digit), `to_upper`, `to_lower` (`→ char`, unchanged when not a letter).
- **Math** — `abs<T extends Ordered & Negatable>`, `min<T extends Ordered>`, `max<T extends Ordered>`, `pow<T extends Ordered & Arithmetic>` (exponentiation by squaring, wraps), `gcd<T extends Arithmetic>` (Euclid, `gcd(0, 0) = 0`). Arithmetic wraps exactly as the language says.
- **`assert(cond: bool)`** — `primitive abort()`s when `cond` is false; it writes no message itself, so print context first if you want any.
- **Heap** — `alloc(bytes: u64) → ptr u8` and `free(p: ptr u8)`: thin wraps over the `alloc`/`free` primitives (`docs/primitive.md#alloc`), so a program reaches the allocator as `std.alloc` / `std.free` like every other std name. The block's contract — zeroed, at least `ceil(bytes / 8)` whole cells — is the primitive's.
