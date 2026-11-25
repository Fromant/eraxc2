# The standard library

`stdlib/std.erx` is the standard library: ordinary erax source compiled with every program. The driver parses it before the source modules. Every module can access its root namespace `std` through qualified names such as `std.print(x)`.

The source of truth is `stdlib/std.erx` itself; it is embedded in the compiler at build time, so compiling never depends on files next to the source.

Alongside it, `stdlib/std.test.erx` is the **std self-test**: ordinary erax, no harness past two counters — `check(cond)` bumps a `checks` counter and a `failed` counter, `main` runs one assertion per behavior, prints the tally, and *returns* the failure count, so the exit code is 0 exactly when the suite passed. It runs anywhere the compiler runs, and the interpreter test suite lowers the checked-in copy (embedded like std itself) and pins its output and exit code.

## Rules

- **std's names are its own namespace.** std's declarations all live inside `namespace std` (`language.md#namespaces`), and the auto-import registers that namespace before the user's declarations: user code reaches std's functions only as `std.…` — there is no bare `print` — and std's names are separate from the user's, so a user function named `abs` or `print` at the root or in a namespace does not clash with std's (`statements.md#namespaces`). Inside `namespace std`, the functions call each other by bare name, exactly as the enclosing-scope rules say. `namespace std` (or another root `std`) may not be reopened by the program.
- **The std namespace is closed, not its generic composition.** User code cannot reopen `std`. Library templates and generic containers may copy, move, and destroy user-defined element types through the same language protocols available to user code. The declaration pre-pass sees the combined library and user token stream; std is not a separately privileged parser phase.
- **No globals, no `main`.** std declares no globals (so it cannot affect a program's observable global state) and never contains the entrypoint.
- Everything std can do, user code can do too: std has no privileges — it uses the same primitives, the same language, and the same templates.

## Contents (v0.1)

Each name below is reached qualified — `std.print`, `std.min`, `std.assert` — and spelled bare here only where it is named as a function.

- **Output** — the number or the word only, no spacing around it: `print` is the template `fn print<T extends Ordered & Arithmetic>(value: T)`, which writes every integer's decimal by recursive division (`value / 10` then one digit per `print_char`). A signed value prints its sign, then its magnitude, read as a wrapping `u64` — `0u64 - (value as u64)` is `i64min`'s magnitude too, so nothing is written out specially. `print(bool)` writes the text `true` / `false`, `print(char)` writes the byte itself, and the zero-argument `print()` writes one newline. Composition — separators, line breaks, columns — is the caller's, written with `std.print(' ')` / `std.print('\n')` or `primitive print_char`. (`primitive print` remains the ready-made "values on one line" case.)
- **Char** — `is_digit`, `is_alpha`, `is_alnum`, `is_upper`, `is_lower` (`→ bool`, ASCII), `to_digit` (`→ i32`, `-1` when not a digit), `to_upper`, `to_lower` (`→ char`, unchanged when not a letter).
- **Math** — `abs<T extends Ordered & Negatable>`, `min<T extends Ordered>`, `max<T extends Ordered>`, `pow<T extends Ordered & Arithmetic>` (exponentiation by squaring, wraps), `gcd<T extends Arithmetic>` (Euclid, `gcd(0, 0) = 0`). Arithmetic wraps exactly as the language says.
- **`assert(cond: bool)`** — `primitive abort()`s when `cond` is false; it writes no message itself, so print context first if you want any.
- **Heap** — `alloc(bytes: u64) → ptr u8` and `free<T>(p: ptr T)`: thin wraps over the `alloc`/`free` primitives (`docs/primitive.md#alloc`), so a program reaches the allocator as `std.alloc` / `std.free` like every other std name. The block's contract — zeroed, at least `ceil(bytes / 8)` whole cells — is the primitive's.

## Ownership and collections

The library's generic structures use the same factories, references, copying,
explicit moves, and automatic destruction as user structures
(`core/statements.md`, `core/types.md`). Their fields are private. They own
storage through raw pointers internally; callers use checked references or
transfer complete values.

### `std.unique<T>`

`std.unique<T>.new(value)` creates one owner of a `T`. A pre-existing value is
copied when passed normally and transferred when passed as `move value`; a
freshly created value can initialize the owner directly. `unique<T>` is movable
and is not clonable. Destroying a nonempty owner destroys its value and frees
its storage exactly once.

| Method | Receiver | Result and behavior |
| ------ | -------- | ------------------- |
| `new(value: T)` | factory | An owner containing `value` |
| `get()` | `ref unique<T>` | `ref mut? T` to the owned value |
| `release()` | `ref mut unique<T>` | Transfers the `T` to the caller and empties the owner |

A released owner remains valid and can be moved or destroyed; its destructor
has no contained value to destroy. Calling `get` or `release` on an
empty owner fails `std.assert`, terminating with code 134. References obtained
from the owner may not be used after release or destruction.

### `std.vector<T>`

`std.vector<T>.new()` creates an empty vector with no allocated element buffer.
The vector owns its initialized elements and releases its buffer on destruction.
Growth transfers existing elements; it does not require `T` to be clonable.
Consequently `std.vector<std.unique<Resource>>` can grow, be returned, and be
passed between functions with explicit `move`.

| Method | Receiver | Result and behavior |
| ------ | -------- | ------------------- |
| `new()` | factory | An empty vector |
| `length()` | `ref vector<T>` | Number of initialized elements, `u64` |
| `capacity()` | `ref vector<T>` | Number of available storage slots, `u64` |
| `empty()` | `ref vector<T>` | Whether the length is zero |
| `reserve(capacity: u64)` | `ref mut vector<T>` | Ensures at least that capacity without changing length |
| `push(value: T)` | `ref mut vector<T>` | Appends a copied or explicitly moved value |
| `at(index: u64)` | `ref vector<T>` | `ref mut? T` to an initialized element |
| `pop()` | `ref mut vector<T>` | Removes the last element and transfers it to the caller |
| `clear()` | `ref mut vector<T>` | Destroys all initialized elements, preserving capacity |
| `clone()` | `ref vector<T>` | A vector with independent storage and copied elements |

Ordinary copying uses the vector's `clone` implementation and requires clonable
elements. Copying a vector of nonclonable owners is a compile-time error.
`at` requires `index < length()`; `pop` requires a nonempty vector.
Violations fail `std.assert` with code 134. Capacity multiplication is checked
before requesting raw allocation; allocator exhaustion remains a machine trap.

A reference to the vector object remains valid when its internal buffer changes.
A reference to an element must not be used after an operation that may invalidate
that element, such as buffer relocation, element removal, or clearing. Element
mutation through a valid mutable reference does not itself relocate the buffer.
Container element destruction order is a library matter, not a language rule;
callers must not depend on an order promised by neither the API nor their own
container implementation.

The implementation stores one source value per 64-bit machine cell, including
record handles. It uses `primitive offset` to address slots and explicit raw
storage moves to transfer initialized values. It does not perform arithmetic on
an executor's pointer encoding (`primitive.md#offset`).

The owning collections use `alloc<T>(count)` and typed freeing. Their capacities count elements.

### `std.string`

`std.string` owns a `std.vector<char>`. Its length, positions, and capacity
count bytes, including embedded NUL. It accepts arbitrary bytes and does not
validate UTF-8. Each string literal creates an independent owner.

| Method | Behavior |
| ------ | -------- |
| `new()` | Creates an empty string |
| `length()`, `empty()` | Return the byte count and whether it is zero |
| `at(index: u64)` | Returns the byte at the zero-based index as `char` |
| `set(index: u64, value: char)` | Replaces the byte at the index |
| `push(value: char)`, `pop()` | Append a byte or remove and return the last byte |
| `clear()`, `reserve(capacity: u64)` | Remove all bytes or ensure at least the requested capacity |
| `append(other: ref string)` | Appends the bytes of another string. Self-append doubles the original contents |
| `concat(other: ref string)` | Returns a new string containing both sequences in order |
| `equals(other: ref string)` | Tests byte-for-byte equality |
| `slice(start: u64, end: u64)` | Copies the half-open byte range `[start, end)` into a new string |
| `clone()` | Returns a copy with independent storage |
| `write()` | Writes every byte to stdout |

`set`, `push`, `pop`, `clear`, `reserve` and `append` require mutable
access. Out-of-bounds indexing, an invalid slice range, and popping an empty
string fail `std.assert` with code 134. String copies use the generated
structural clone of the backing vector.

`std.print("text")`, `std.print(ref text)`, and `text.write()` write the
bytes without adding a newline. Use `std.print()` to add a newline.

## Result

`std.Result<T, E>.ok(value)` and `std.Result<T, E>.err(error)` create an owned
result with one active payload. `is_ok()` reports the original branch.
`take_value()` and `take_error()` require mutable access and transfer the
corresponding payload once. Calling either accessor for the wrong branch or
after taking the payload aborts. Destruction releases any payload still held.
Results support `move`. They have no `Clonable` implementation.
