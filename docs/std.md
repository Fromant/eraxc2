# The standard library

`stdlib/std.erx` **is** the standard library: ordinary erax source, compiled with every program. The driver parses it **before** the file it was given (auto-import), so its declarations are part of every program and a program simply calls them. When the import system arrives (`TODO.md`), auto-import goes away and std becomes an ordinary imported module — nothing in this file's *contents* changes, only how it gets in.

The source of truth is `stdlib/std.erx` itself; it is embedded in the compiler at build time, so compiling never depends on files next to the source.

## Rules

- **std's signatures are taken.** std's functions land in the same flat namespace as the user's (v1 has no namespaces), and std is parsed first — so redeclaring one of its signatures, e.g. `fn print(x: i32)`, is the ordinary duplicate-declaration error. A name is not reserved as a whole: a declaration that adds a *new* parameter list to a std name (say, `fn print(x: i32, y: i32)`) joins that overload set rather than clashing — only a repeated signature clashes (`statements.md#functions`).
- **std is closed.** It may call primitives and other std functions, never user code: it is fully parsed before the user's declarations exist, so the reverse could not resolve anyway.
- **No globals, no `main`.** std declares no globals (so it cannot affect a program's observable global state) and never contains the entrypoint.
- Everything std can do, user code can do too: std has no privileges — it uses the same primitives and the same language.

## Contents (v0.1)

- **Output** — the number or the word only, no spacing around it: `print` has one declaration per type — `i8` … `i64`, `u8` … `u64` (the decimal its type reads), `bool` (the text `true` / `false`), `char` (the byte itself) — plus the zero-argument `print()`, which writes one newline, and `put_space()` / `put_newline()`. Composition — separators, line breaks, columns — is the caller's, written with `primitive print_char`. (`primitive print` remains the ready-made "values on one line" case.)
- **Char** — `is_digit`, `is_alpha`, `is_alnum`, `is_upper`, `is_lower` (`→ bool`, ASCII), `to_digit` (`→ i32`, `-1` when not a digit), `to_upper`, `to_lower` (`→ char`, unchanged when not a letter).
- **Math** — `abs_i64`, `min_i64`, `max_i64`, `min_u64`, `max_u64`, `pow` (`u64 × u64 → u64`, wraps), `gcd` (`u64`, `gcd(0, 0) = 0`). Arithmetic wraps exactly as the language says.
- **`assert(cond: bool)`** — `primitive abort()`s when `cond` is false; it writes no message itself, so print context first if you want any.
