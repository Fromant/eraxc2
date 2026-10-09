# The standard library

`stdlib/std.erx` **is** the standard library: ordinary erax source, compiled with every program. The driver parses it **before** the file it was given (auto-import), so its declarations are part of every program and a program simply calls them. When the import system arrives (`TODO.md`), auto-import goes away and std becomes an ordinary imported module — nothing in this file's *contents* changes, only how it gets in.

The source of truth is `stdlib/std.erx` itself; it is embedded in the compiler at build time, so compiling never depends on files next to the source.

## Rules

- **std's names follow the ordinary name rules.** std's functions land in the same flat namespace as the user's (v1 has no namespaces), and std is parsed first. A template reserves its name for exactly one template — declaring a second is the ordinary duplicate-declaration error (`statements.md#templates`). A *concrete* declaration is a new parameter list: it joins the name rather than clashing — so `fn print(x: i32, y: i32)` and even `fn print(x: i32)` coexist with the `print` template — and only a repeated signature clashes (`statements.md#functions`).
- **std is closed.** It may call primitives and other std functions, never user code: it is fully parsed before the user's declarations exist, so the reverse could not resolve anyway.
- **No globals, no `main`.** std declares no globals (so it cannot affect a program's observable global state) and never contains the entrypoint.
- Everything std can do, user code can do too: std has no privileges — it uses the same primitives, the same language, and the same templates.

## Contents (v0.1)

- **Output** — the number or the word only, no spacing around it: `print` is the template `fn print<T extends Ordered & Arithmetic>(value: T)`, which writes every integer's decimal by recursive division (`value / 10` then one digit per `print_char`). A signed value prints its sign, then its magnitude, read as a wrapping `u64` — `0u64 - (value as u64)` is `i64min`'s magnitude too, so nothing is written out specially. `print(bool)` writes the text `true` / `false`, `print(char)` writes the byte itself, and the zero-argument `print()` writes one newline. Composition — separators, line breaks, columns — is the caller's, written with `print(' ')` / `print('\n')` or `primitive print_char`. (`primitive print` remains the ready-made "values on one line" case.)
- **Char** — `is_digit`, `is_alpha`, `is_alnum`, `is_upper`, `is_lower` (`→ bool`, ASCII), `to_digit` (`→ i32`, `-1` when not a digit), `to_upper`, `to_lower` (`→ char`, unchanged when not a letter).
- **Math** — `abs<T extends Ordered & Negatable>`, `min<T extends Ordered>`, `max<T extends Ordered>`, `pow<T extends Ordered & Arithmetic>` (exponentiation by squaring, wraps), `gcd<T extends Arithmetic>` (Euclid, `gcd(0, 0) = 0`). Arithmetic wraps exactly as the language says.
- **`assert(cond: bool)`** — `primitive abort()`s when `cond` is false; it writes no message itself, so print context first if you want any.
