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
- [x] Templates — `fn name<T extends Ordered & Negatable>(…)` with type arguments deduced from the call, capability bounds checked at the call site, and one monomorphized function per distinct instantiation (`docs/core/statements.md#templates`, `docs/core/types.md#capabilities`, `docs/core/expressions.md#typing-rules`).
- [x] Namespaces — `namespace NAME { … }` blocks (nestable), dot-qualified references (`std.print(x)`), the closed `namespace std` as the auto-imported stdlib, and per-namespace name/clash rules. JIR stays nameless; templates instantiate inside the namespace that declared them (`docs/core/language.md#namespaces`, `docs/core/statements.md#namespaces`).

## Ownership and RAII

Accepted design from the language discussion. This section records the implementation target, not features already available in `docs/core/`. No VM or AoT work is part of this change. The end-to-end target is a `std.vector<std.unique<Resource>>` passed between functions, with correct cleanup on all ordinary exits.

### Implementation status

- [x] Fix heap dereference of freed blocks and zero reused allocations; regression tests cover both.
- [x] `const` versus `let`, independent `mut` binding metadata, local type inference, and explicit moves of locals/parameters.
- [x] Source initialization events checked to a fixed point over the CFG, including loop back edges after moves.
- [x] Right-associated statement-only chains of plain `=`, including raw dereference targets.
- [x] Recursive template signature types (`ptr T`, `ptr ptr T`) and independent `Clonable` / `Movable` capability bounds for the existing scalar/pointer types.
- [ ] Named generic structures, layout, fields, factories, methods, and visibility.
- [ ] Safe `ref` / `ref mut`, reference fields, inferred lifetime dependencies, and invalidation effects across calls.
- [ ] User-defined copying, automatic structural capabilities, RAII destruction, and temporary ownership.
- [ ] `std.unique<T>`, `std.vector<T>`, and the target conformance programs.
- [ ] Ordinary fallible factory results (`Result` needs a concrete representation and syntax); do not implement exceptions.

### Bindings and access

`const` prohibits rebinding; `let` permits it. `mut` grants object mutation independently: `const mut v = ...` fixes the owner but permits changing the vector; `let v = ...` permits replacement but not mutation through that binding. No `readonly` keyword. Access without `mut` does not permit mutation through owned fields or owned pointees, nor extraction of mutable access through reference fields. It does not assert that no other alias can change the object.

`ref object` and `ref mut object` explicitly create safe references. No implicit borrowing in ordinary initialization. Copying a reference preserves its target rather than copying the target. Mutable-reference aliases are permitted; Rust-style exclusive mutable borrowing is not the accepted model. A reference promises a valid resource at use. Ordinary mutation is permitted through other authorized access; destruction, replacement, relocation, and container operations that invalidate a referenced element must be checked against all still-used references. References to a vector object and references to its elements have different invalidation dependencies. Last-use analysis, not merely lexical scope, determines reference use. Reference fields are supported; dependencies survive packaging, copying, moves, returns, and collection storage. No user lifetime parameters in this implementation stage. A returned reference conservatively depends on the reference arguments; refine summaries where feasible. Self-referential object creation is excluded until stable storage and movement are specified.

Raw `ptr` is unsafe, `ref` is safe. There is no `unsafe` keyword or block. Ordinary casts cannot manufacture a safe reference from arbitrary raw bits or a raw pointer. Code using raw pointer operations is responsible for preserving any live safe-reference contract.

### Structures and creation

All fields, methods, and factories are `private` by default; `public` and `private` apply to each. No inheritance, `protected`, or separate `impl` blocks. An explicitly public field can be changed through mutable access; its author is responsible for the exposed invariant. Receivers are written `this`, not `self`, with explicit `ref Type` or `ref mut Type` types.

```
struct Buffer {
    data: ptr u8;
    size: u64;

    public new(size: u64): Buffer {
        return { data: primitive alloc(size), size: size };
    }

    public fn length(this: ref Buffer): u64 {
        return this.size;
    }

    destructor {
        primitive free(this.data);
    }
}
```

`Buffer.new(...)` invokes a factory. An object literal `{ field: expression, ... }` inside the type's factory assembles the value directly without recursively invoking that factory. No `init` keyword and no general external `Buffer { ... }` construction. Every field is initialized exactly once; no accessible partially initialized `this`. Only the type's factories can assemble fields. Overload `new` by parameter types; named factories are ordinary receiverless methods that invoke `new`. Do not synthesize an unrestricted factory. Generic type factory calls use `std.vector<i32>.new()`; type parsing must handle nesting without changing maximal-munch operator tokenization.

The factory may return an ordinary result carrying an error. Its failure path destroys already-created locals and temporaries. Fields evaluate in written order. Assembly from prepared fields introduces no independent hidden failure. Construction directly in final storage does not require copying or moving a pre-existing value. Temporaries last through their containing expression; saving a reference to a temporary for later use is rejected.

### Copying, moving, assignment

Ordinary initialization from an existing value and plain assignment copy; there is no implicit move. User `clone` implements the copying protocol; a separate `clone` keyword is unnecessary. A type may support either, both, or neither `Clonable` and `Movable`. Do not require repetitive explicit trait lists on every type. Simple structural copying is generated when fields permit it and there is no user destructor. A destructor disables automatic shallow copying, not automatic movement. `unique<T>` is not clonable; consequently neither is `vector<unique<T>>` without another explicit resource-copying contract.

`move value` transfers a whole value and leaves its source uninitialized, rather than invoking a user-defined move method or requiring an empty state. `const` values can move; they cannot be reinitialized. Movability is checked independently of clonability and reference invalidation. Partial field moves are not supported in this stage.

Assignment prepares the RHS before destroying the previous destination, then installs the new value. Reject a new value that would retain a safe reference to the destroyed destination. `a = a` copies before replacement; `a = move a` is an error. Plain `=` chains associate right to left and remain statement-only. Outward links copy the inner assigned value; a chain requiring a copy of a non-clonable owner must be rejected. Associativity does not replace explicit evaluation/cleanup ordering.

### Destruction

Destruction is universal, not an optional `Droppable` implementation. Optional `destructor { ... }` adds user cleanup. Run its body, then destroy owned fields in reverse declaration order. Locals are destroyed in reverse order of successful initialization on fallthrough, `return`, `break`, and `continue`. Prepare a returned value before local cleanup. By-value parameters belong to the callee unless moved out. Destroy global owners in reverse initialization order after normal `main` completion. Traps, `abort`, and immediate `exit` do not unwind.

The language does not prescribe the order in which a container destroys its elements: that belongs to std or the particular collection. A destructor must not move the entire dying `this` or leak a safe reference to it. Forgetting a user destructor must not disable automatic field destruction. Manually managed raw pointers still require a correct manual resource contract.

### Architecture and remaining layout decisions

Introduce a typed representation of storage places and initialization/copy/move/borrow/destroy operations before expanding parser-side cleanup logic. Lower cleanup to ordinary calls and CFG edges, not a backend-specific RAII mechanism. Extend semantic types with named generic applications and references. Resolve representation, allocation size/stride, addressable fields and elements, and aggregate call/return lowering before implementing vector storage; do not encode the interpreter's region/index layout in pure-erax pointer arithmetic. Concrete memory/layout changes may require JIR and reference-interpreter work but no VM/AoT development.

Safe generic containers must invoke user element destruction/copy operations, so replace the obsolete std rule forbidding every call into user code with protocol-based composition. Specify how library raw-storage operations establish and invalidate safe element references before claiming memory safety. Explicit opt-out syntax for automatically inferred capabilities, non-relocatable storage, and precise reference-field effect summaries remain design work.

## Beyond v1 (future work)

Source-level additions that are out of scope for v1 — listed here so the language contract (`docs/core/`) stays scope-clean:

- `?:` (ternary), optional chaining (`?.`), spread (`...`), ranges, `::` namespace resolution (dots remain the namespace syntax), implicit widening.
- Short-circuit `&&` / `||` — v1 evaluates both operands (see `docs/core/expressions.md`). It needs an op the current table has none of (a conditional-jump or select form), so it is a `.jir` version bump.
- An `unreachable code` diagnostic — v1 parses dead code, checks it, and drops it.
- Strings and string literals, floats — and with them string support in the stdlib (`std.print`); v0.1 std lives in `stdlib/std.erx` already (`docs/std.md`). `examples/helloworld.erx` is aspirational on `string`.
- Machine-level futures — arrays/indices, references, objects/classes, closures — live in the JIR section below. Raw pointers (`ptr T`, `&`, `*`) and the `alloc`/`free` heap are implemented.
- **A `char` suffix.** `char` is a type name and `is_integer(char)` holds, but it is not a literal suffix (`types.md#literals`), so a *constant* `char` expression cannot be written: `let x: char = 255 / 16;` is an `i32` division in a `char` position. `(255 / 16) as char` already spells the value (`types.md#casts`), but only a suffix makes the arithmetic itself read as `char` arithmetic instead of `i32` ending in a cast — adding `char` to the suffix set closes that.
- **Literal / singleton types.** `let b: i32 = 0;` gives `b` the type `i32`, not the value `0`. A type system that tracked the value — so `b` has type `0` and a later `b = 2` is an error until the use — would catch the divide-by-a-literal-zero and the accidental-overflow class of bug at compile time instead of at the trap, and it is the natural end state of the constants work in `types.md#constants`. It costs a real type-inference term and is far past v1.
- **More template machinery.** v1 templates (`docs/core/statements.md#templates`) are deliberately minimal: **one template per name**, type arguments **always deduced**, and a **closed** capability bound vocabulary (`docs/core/types.md#capabilities`). Each extension has a cost. A second template per name needs a rule for which one a call picks — v1 says the name decides, and a concrete declaration that matches exactly wins. **Explicit instantiation** (`f<u8>(x)`) reopens the ambiguity that kept it out: `a < b > (c)` is an unknown-operator error today (`docs/core/language.md#operator-set`), and making it a call needs either a `::`-style token (banned — dots are the namespace syntax) or a grammar rule the language does not have. A **wider capability vocabulary** veers into a full interface system — `Equatable`, `Bitwise`, ordering beyond `<`, and user-declarable bounds and the types that name them — which is real type-system work well past v1.
- **Definition-time template body checking.** v1 type-checks a template's body **once per instantiation**, against the deduced concrete type. A better design checks it **once, against the bound**, so a body that uses an operator the bound does not promise is an error at the declaration, not at the first call. That needs the checker to reason about "some `T` satisfying these capabilities" — a type variable in the type system — which the concrete-per-instantiation pass sidesteps.
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
- Strings — `std.string` in the stdlib, a heap-pointer wrapper; **no string ops in JIR, ever**. It sits on the raw pointers below.
- Ownership / RAII (drop, moves, `std.unique`), arrays/indices, references — the uses that pointers currently leave to the program: a pointer is a raw cell with no lifetime, so forgetting a `free` leaks and remembering it twice traps. A managed form (free exactly once, automatically) is the future work; raw pointers stay available underneath.
- Closures, objects/classes.
- Imports — `std.erx` is auto-imported by the driver today (`docs/std.md`); a real import system replaces that. Dot access already works: the auto-imported stdlib is the root namespace `std`, reached as `std.…`.
- New primitives — each is a catalog addition in `docs/primitive.md`, never a language change: memory (`alloc`/`free`) landed; raw byte output (`write_bytes`, what strings will print through), and friends. Time/random stay excluded: the determinism rule in `docs/primitive.md` is what makes a backend comparison mean anything.

## Backends

- [ ] **VM** — executes JIR from a serialized form.
- [ ] **AoT** — native codegen. Codegen concerns (instruction selection, frame layout, register allocation) are backend work.

The driver contract for both is **flag = backend** (decided): `eraxc file.erx` runs on the reference interpreter and stays the default; `--vm` / `--aot` run with that backend; `-o out.jir` / `-o out.exe` emit the artifact instead of running and imply the backend. The `Action`/backend split in `main.cpp` already leaves the slot. **The stdlib is pure erax** — `stdlib/std.erx`, embedded and auto-imported (`docs/std.md`) — so every backend runs it without implementing anything for it. What a backend *does* implement is the **primitive catalog** (`docs/primitive.md`): the interpreter, VM, and AoT all satisfy the same five contracts the same way, which is exactly what makes their results comparable.

## Serialization (`TODO.md` owns the format versioning)

`.jir` = magic + `u32` version + deterministic little-endian encoding of `Program` (commands as op + operand list; arity from the op table; a `PRIM` carries the primitive's catalog `u32` id). The op table **and the primitive catalog** are frozen by the format — a new op or primitive is a version bump. The pointer phase added three ops (`LOAD`/`STORE`/`ADDR`), the `ptr T` type, and two primitives (`alloc`/`free`); when serialization lands it must carry a version past the one the five-primitive, nineteen-op set was frozen at.

## Conformance

Programs under `examples/` double as the conformance corpus: same files, every backend. Results are compared as **the bytes the program wrote to stdout (`print` / `print_char`) + final global state + exit code** (the entrypoint's return, or the code `exit`/`abort` gave); a program that uses `read_char` is run with pinned input. Strings, formatting, and `std.print` still arrive later.

**Pointer caveat.** A pointer's *value* is the executor's address encoding (`docs/middleend/JIR.md#runtime-semantics`): identity (`p == q`) is conformance-safe, but a program that prints an address or otherwise lets a specific `&`/`alloc` value out of its program is not — the corpus's pointer programs compare blocks and identity only. Whether raw address values should be made deterministic (or excluded from conformable programs) is open.