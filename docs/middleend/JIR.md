# Middleend — JIR

JIR is the single IR of erax: what the parser emits and what every backend executes. It is a register-style, stack-framed IR — a program runs on one machine stack of cells, so a compliant executor is a stack machine by construction (an array of cells plus a frame pointer is the whole structure), with the heap `alloc`/`free` hand blocks out of as a second region of the same array.

Roadmap and format versioning live in `TODO.md`; this file is the language contract only.

## Grammar

```
program     = global* function* entrypoint
global      = type , optional init cfg          ; empty init = zero-initialized
function    = function id , return type , param types , frame size , cfg
entrypoint  = function id                       ; zero params, returns i32
command     = OP operand*                       ; args[0] is the result slot of ops that produce one
             | "PRIM" primitive operand*        ; primitive = a catalog name; args[0] is the result slot
```

A function's **frame size** is the number of cells its frame occupies — one more than its highest vreg id. All control flow between blocks lives on CFG edges; there are no jump commands.

## Lexicon

Every operand is `(kind, type, value)` — self-describing; nothing is resolved at load or run time.

| Operand | Print        | Value means                                   |
| ------- | ------------ | --------------------------------------------- |
| Instant | `type value` (e.g. `i32 5`, `u8 1`) | the immediate                          |
| VReg    | `type $k` (e.g. `i32 $3`) | cell `fp + k` of that type in the current frame |
| Global  | `@k`         | cell `k` of the process-wide globals array    |
| Fn      | `&k`         | function id `k`                               |
| EMPTY   | `EMPTY`      | the empty sentinel — names "no cell" (see [calls](#calls)) |

- A cell operand prints its type, so an instruction is self-describing: an **Instant** and a **VReg** read
  `type value` / `type $k` (`i32 5`, `i32 $3`). A `Global` and a `Fn` print their id alone (`@0`, `&k`) because
  the id already names the cell — `program.globals[k].type`, `program.functions[k].return_type` — and those
  tables ship with the IR.
- A `VReg` id is an **offset within the current frame**, always `0 ≤ k < frame_size`.
- A `Fn` operand carries the callee's return type; a `Global` operand carries the global's type.
- An `Instant`'s value is the **raw bit pattern** of the cell it fills; its printed form is interpreted with the type's signedness (`i8 0xFF` prints as `i8 -1`).
- The `EMPTY` sentinel is `(VReg, void, u64(-1))` — a phantom offset that names "no cell". It is legal only as the result slot of a `CALL` or a `PRIM`.

## Types

| Type           | Bytes |
| -------------- | ----- |
| `void`         | 0     |
| `i8` `u8`      | 1     |
| `i16` `u16`    | 2     |
| `i32` `u32`    | 4     |
| `i64` `u64`    | 8     |
| `ptr T`        | 8     |

- `bool` lowers to `i8`; `char` lowers to `u8` — it is a `0–255` byte, so its comparisons and its `/ % << >>` are unsigned.
- Source records lower to an owned `ptr u64` handle to a block of field cells;
  every field uses one storage cell, including nested record handles. Safe
  references lower to pointers to the target's lowered cell type. Nominal record
  identity, reference permissions, initialization and lifetime constraints are
  checked before lowering; JIR adds no record or reference type kind.
- `ptr T` is a pointer to a cell holding `T`; the pointee is part of the type (`ptr i32` ≠ `ptr u8`), and a pointer is one word whatever it points at. It is built with `Type::pointer(T)`; it prints `ptr T`; and a `LOAD` reads through it in `T`'s type.
- No implicit conversions: where an op requires matching types they must match exactly (the validator enforces this).

## Frames

- VReg ids address the **current frame** only: `$k` is the cell at `fp + k`.
- A `CALL` pushes a frame of the callee's `frame_size` cells and copies the arguments into its leading slots `$0..$n-1`; a `RET` pops it. Cells are consequently **killed on return** — locals live only while their function is on the stack.
- Recursion is ordinary: each activation has its own cells, and the same id in a nested activation addresses a different cell. The stack cost of a path is the sum of the nested frames' sizes.
- The frame layout is static: `frame_size` is a compile-time constant per function, so every activation costs a known number of cells. A function that uses no vregs still has `frame_size` 1 — a frame is never empty.

## Commands and control flow

- One shape for every command: `OP arg0 arg1 …`. Roles come from the op table; an op that produces a result writes `args[0]` (a vreg, or for `MOV` also a global).
- A function body (and a global initializer) is a **CFG**: nodes are ordered lists of commands; node `0` is the entry. Every node has **0 to 2 outgoing edges**:
  - `0` — the block ends in `RET`, or in a terminating `PRIM` (`exit`/`abort`);
  - `1` — unconditional;
  - `2` — a branch: one edge carries a `Condition`, the other is the fallthrough.
- A `Condition` is `(cmp, a, b)` with `cmp ∈ CMP_EQ…CMP_GE` — the same ops as the `CMP_*` commands, drawn from the one op table — e.g. "`x` is nonzero" is `(CMP_NE, x, 0)`. Execution follows the edge whose condition holds, else the fallthrough.
- There are **no jump commands**.

## Operations

### Data movement

Source-level `move local` lowers to a `MOV` into a fresh result cell. Consumption,
`const` rebinding restrictions, and the `Clonable` / `Movable` bounds are checked
by the frontend; they do not add opcodes, clear the source cell, or change the
machine's scalar calling convention. The frontend checks initialization across
CFG back edges before discarding unreachable blocks.

Factories, methods, clone implementations and destructors are ordinary functions.
The frontend lowers automatic destruction to calls guarded by initialization
flags and CFG edges. A record drop wrapper invokes the user destructor, destroys
owned fields in reverse declaration order, then releases the field block. A
generated entrypoint wrapper runs ordinary `main`, preserves its result, destroys
global owners in reverse initialization order, and clears their cells. Immediate
termination and traps bypass these ordinary control-flow paths. Record global
initializers may contain calls and have the usual inferred initializer frame.

| Op    | Shape      | Semantics                                                    |
| ----- | ---------- | ------------------------------------------------------------ |
| `MOV` | `MOV dst src` | `dst = src` (`src`: instant/vreg/global; `dst`: vreg/global). The only way to read or write a global. |

### Conversions

| Op     | Shape         | Semantics                                                    |
| ------ | ------------- | ------------------------------------------------------------ |
| `CAST` | `CAST dst src` | read `src` in **its** type, then keep the low bits of its two's-complement pattern at the width of **`dst`**'s type (wrap). The two operand types are the explicit ones on the operands: they may differ (that is the point) or be equal (a no-op). No `void` — `need_value` covers it. |

A cast to the source's `bool` never reaches the IR as `CAST`: `bool` *is* `i8` here, so `CAST … i8` could not tell `x as i8` from `x as bool`. The parser lowers a to-`bool` cast to the compare the machine already spells nonzero with — `CMP_NE dst src 0` (the same shape [`!` uses](#comparisons)) — and a from-`bool` value is an ordinary `i8` 0/1 cell, so every other direction is a plain `CAST`.

A `CAST` to or from a pointer follows the same rule: the operand's pattern is kept at the target's width. `src` read as the unsigned word its type reads, so `0 as ptr i32` is the all-zero address (the pointer that names nothing), and `ptr u8 as u64` is the address's bits — the only place an address leaves the pointer's own type.

### Pointers

Four ops operate on pointers. The pointer cell itself is produced by `ADDR` or by `PRIM alloc`; a register/global that holds bit patterns (`0 as ptr i32`, a `ptr` parameter) is an ordinary cell.

| Op      | Shape             | Semantics                                                    |
| ------- | ----------------- | ------------------------------------------------------------ |
| `LOAD`  | `LOAD dst src`    | `dst = *src`: `src` is a pointer, the result is `src`'s pointee (the validator checks `dst.type == pointee(src.type)`). A read through an address that names no live cell is a trap. |
| `STORE` | `STORE ptr val`   | `*ptr = val`: no result slot — `args[0]` is the pointer, `args[1]` the value, and the validator checks `pointee(ptr.type) == val.type`. A write through an address that names no live cell is a trap. |
| `ADDR`  | `ADDR dst src`    | `dst = &src`: `src` is a **vreg or global** lvalue (never a constant or a scratch cell), `dst` is `ptr` to its type. |
| `OFFSET` | `OFFSET dst ptr index` | Advance by `index: u64` storage cells inside the same live allocation; the result preserves the pointer type. |

A compound store (`*p += v`) is `LOAD` into a temp, the operator, then `STORE` — the same three-instruction shape a named `x += v` has.

`OFFSET dst ptr index` advances a pointer by an unsigned `u64` cell count. `dst` is a vreg of exactly `ptr`'s pointer type. The value width (`size_of`) does not determine the storage stride: every storage slot is one 64-bit cell. The source and result must be live cells in the same allocation, so the operation cannot cross a heap block, source stack frame, or single-cell global. Null, dead, one-past, and overflowing offsets trap. Zero offset preserves a live pointer. The primitive `offset` shares this operation with a `ptr u8` signature (`docs/primitive.md#offset`). Pointer arithmetic therefore needs no knowledge of an executor's address encoding.

### Integer arithmetic

Two's-complement, wrapping. Signedness of div/mod and `SAR` comes from the operand type.

| Op                 | Shape         | Semantics                            |
| ------------------ | ------------- | ------------------------------------ |
| `ADD` `SUB` `MUL`  | `ADD dst a b` | `dst = a op b` (wrap)                |
| `INC` `DEC`        | `INC dst a`   | `dst = a ± 1` (wrap)                 |
| `NEG` `NOT`        | `NEG dst a`   | `dst = -a` / bitwise `~a`            |
| `AND` `OR` `XOR`   | `AND dst a b` | bitwise                              |
| `SDIV` `SMOD`      | `SDIV dst a b`| signed div/mod, truncate toward zero |
| `UDIV` `UMOD`      | `UDIV dst a b`| unsigned div/mod, truncate toward zero |
| `SHL` `SHR` `SAR`  | `SHL dst a b` | shift by `b`; `SHR` logical, `SAR` arithmetic |

Divide-by-zero and `iMIN / -1` terminate the process (see [semantics](#runtime-semantics)).

The source's logical operators lower to these bitwise ops on `i8` 0/1 operands, where the two meanings coincide: `&&` → `AND`, `||` → `OR`, `^^` → `XOR`. All three are **eager** — both operands are always evaluated, so no expression needs control flow and every expression lives in one block.

### Comparisons

`dst` is always `i8` holding 0/1; signedness comes from the operand type.

| Op                                                       | Shape          | Semantics         |
| -------------------------------------------------------- | -------------- | ----------------- |
| `CMP_EQ` `CMP_NE` `CMP_LT` `CMP_LE` `CMP_GT` `CMP_GE`    | `CMP_EQ dst a b` | `dst = (a op b)` |

Pointers take part in `CMP_EQ`/`CMP_NE` only, and those compare as the unsigned words their addresses are — the one question the IR lets a program ask of a cell. The language never lowers an ordered pointer comparison (ordering a pointer is a type error at the source, `expressions.md#typing-rules`); an IR hand-written with `CMP_LT` on two `ptr` cells is still executed as an unsigned compare, but no source produces one.

Logical negation is `CMP_NE i8 $dst i8 $a i8 0`.

### Control

| Op     | Shape                              | Semantics                                                  |
| ------ | ---------------------------------- | ---------------------------------------------------------- |
| `CALL` | `CALL dst &f a0 … an-1`            | call `f`, see [calls](#calls)                              |
| `PRIM` | `PRIM p dst a0 … an-1`             | run primitive `p`, see [primitives](#primitives)           |
| `RET`  | `RET src` / `RET`                  | return `src` (non-void) / nothing; a `RET` block has no edges |

## Calls

`CALL` has exactly one shape: `args[1]` is the callee, `args[0]` is the result slot, `args[2..]` are the arguments. Decoding never depends on the callee's signature — the call itself declares the shape.

Executing `CALL dst &f a0 …`:

1. push a frame of `f.frame_size` cells;
2. copy `a0..an-1` into `$0..$n-1` of the new frame (the caller's cells are untouched — the callee reads copies);
3. run `f`'s CFG;
4. on `RET`, write the returned value to `dst` in the caller's frame and pop the frame.

A void callee is signaled by `dst` being the `EMPTY` sentinel: value is discarded (a sentinel anywhere else is a validator error).

**Example.** `fn add3(x: i32, y: i32, z: i32): i32 = x + y + z`; in `main`, `a=$0`, `b=$1`, `c=$2`:

```
CALL i32 $3 &add3 i32 $1 i32 1 i32 $0   ; r = add3(b, 1, a)
```

A void call — a helper `log(x: i32)` — states its own shape:

```
CALL EMPTY &log i32 $3                  ; EMPTY dst — no value comes back
```

## Globals and startup

1. Every global is **zero-initialized**.
2. Each non-empty `init cfg` runs in declaration order (a normal CFG — zero params, globals pre-defined).
3. The entrypoint runs; its `i32` return value is the process exit code.

## Primitives

`PRIM p dst a0 …` hands control to the backend-provided primitive named `p`. The catalog — names, ids, signatures, and the exact behaviour of every primitive — is the language-level contract in `docs/primitive.md`; this section is only how the instruction is shaped and checked.

- `p` is a name from the fixed catalog. The text form spells the name; the serialized form stores the primitive's **id** (catalog order, frozen with the format).
- `args[0]` is the result slot: the `EMPTY` sentinel iff the primitive returns void, otherwise a vreg of its return type — the same rule as `CALL`. `args[1..]` are the arguments; their count and types must match the primitive's signature. The parser has already resolved lone literals to their parameter types, so the IR carries exact types and no rule about literals survives lowering.
- `print` is the one variadic primitive: any number of value operands, none of them `void` (arity `0..∞`), `dst` `EMPTY` — every operand is a read.
- `print` and `print_char` write to stdout; those bytes are part of the observable result ([execution model](#execution-model)). `exit` and `abort` terminate the run: a block ending in either has no outgoing edges, like a `RET` block.
- `alloc` and `free` move blocks through the heap: `alloc` hands back a pointer to `bytes` worth of zeroed cells (rounded up to whole cells, at least one), `free` returns a live pointer's block to the allocator and traps on one that names nothing. They are the primitive pair [pointers](#runtime-semantics) sit on; their exact contract is `docs/primitive.md`.
- `PRIM` is the one way a program touches the world. String literals lower to ordinary calls that construct and fill `std.string`. The library writes their bytes through `PRIM print_char`.

## Runtime semantics

Every value is defined in this contract or the program is rejected by the validator — there is no undefined behavior.

- **Overflow wraps** (two's complement): `i64max + 1 = i64min`. `CAST` wraps the same way by definition — it always lands *in* the target's type, wide or narrow.
- **Divide-by-zero = process death**; `iMIN / -1` dies too.
- **Div/mod truncate toward zero**, signed and unsigned.
- **Shift counts wrap to the operand width**: `count & (width-1)` — `i8`/`u8` → 3 bits, `i16`/`u16` → 4, `i32`/`u32` → 5, `i64`/`u64` -> 6.
- **Signedness**: signed for `i*`, unsigned for `u*` (comparisons, div/mod, `SAR`); a pointer reads as an unsigned word, so pointer comparisons are the unsigned ones.
- **Pointers name cells.** A pointer cell is an address the executor encodes from `{cell, region}` — the IR does not fix the encoding, only its behavior: two pointers compare equal iff they name the same cell, deref through the pointer reads/writes that cell in the pointee's type, and a pointer that names nothing is an error on use. In the reference interpreter the address is `region << 62 | index` for one of three regions — the current frame's stack, the globals table, or the heap — with region `0` the null pointer; a backend may encode its cells differently as long as the behavior matches.
- **A dead pointer is process death.** `LOAD`/`STORE` (and `PRIM free`) through an address that names no live cell — the null pointer, an address in a frame that has returned, a block already freed, an out-of-range heap index — terminates the run the same way a divide does. So does an `alloc` the machine's cell array cannot hold. These are the reference interpreter's `InvalidPointer` and `OutOfMemory` traps.
- **Bool**: `i8` 0/1; a condition is always a `(cmp, a, b)` pair with C's "nonzero" rule.
- **Determinism**: ordered IR, single-threaded. The same program has the same observable result (the bytes written to stdout, final global state, the exit code — the entrypoint's return or the one `exit`/`abort` gave) on every backend. Pointer *identity* is part of that result; a specific cell's address is not — it is the executor's encoding, so a program that turns addresses into integers and prints or further compares them leaves the determinism guarantee (`TODO.md#conformance`).

## Validator

Runs on every `Program` before any backend sees it. A producer leaves the CFG canonical: it drops the blocks no edge can reach (the parser knows which source constructs cannot run) and never lets a path fall off the end of a function.

1. **CFG**: entry block is 0; every block reachable; every edge's `from`/`to` valid; 0–2 outgoing edges per block; a two-edge block has exactly one conditional edge; no outgoing edges iff the block ends in a `RET` or a terminating `PRIM` (`exit`/`abort`), and that command is the block's last command.
2. **Commands**: arity and operand roles match the op table; a `PRIM`'s name, result slot, and arguments match its catalog entry (signature, `EMPTY`-iff-void, `print`'s variadic values); operand types are the explicit ones on each operand; ops requiring same-type operands get them — `CAST` is the one op whose two operands differ on purpose; no implicit conversions. The pointer ops carry their own rule: `LOAD`'s result is the source's pointee, `STORE`'s value is its pointer's pointee, and `ADDR`'s source is a vreg or global lvalue whose type is `dst`'s pointee.
3. **Frames**: a function's frame is never empty and holds its params in `$0..n-1`; every vreg id `< frame_size`.
4. **Definitions**: every vreg read has a reaching definition from entry (params and globals count as defined at entry); every vreg has one consistent type across all reads and writes.
5. **Calls**: `args[1]` is a `Fn` operand naming a valid function — the one slot an `Fn` may sit in; `args[2..]` count and types match the callee's params and are all defined; `args[0]` is the `EMPTY` sentinel iff the callee returns void — a sentinel anywhere else is an error, and a non-void `dst` matches the callee's return type.
6. **Program**: entrypoint exists, zero params, `i32` return.
7. **Init CFGs**: the same rules as function CFGs, zero params, and reads nothing.

A branch test is a `CMP_*` on two values of one type, and its operands are read like any other: in frame, and defined on the way here.

Signedness is in the op's name, so the operands have to agree: `SDIV`, `SMOD`, `SAR` signed; `UDIV`, `UMOD` unsigned. `SHL` and `SHR` are defined either way. A comparison answers `i8` regardless of what it compares, since its signedness comes from its operands.

## Execution model

- **Interpreter / VM**: one array of cells; a frame pointer (plus a stack pointer) walks frames — `CALL` pushes, `RET` pops. The engine is a stack machine and nothing else.
- **AoT**: frames map to native stack frames; vregs resolve to frame-pointer-relative addresses; instruction selection, frame layout, and register allocation are backend concerns.
- Serialized `.jir` format, magic, and versioning: see `TODO.md`.

The interpreter of `src/interp/` is this contract made executable, and its results are what a backend is compared against. It is where the parts of the contract that leave room to the executor are pinned down:

- **A cell is one `u64`** holding the raw bit pattern of whatever type the IR gave it. A value is never held in a host type, so narrowing and wrapping are the IR's business: an `i32` cell is zero-extended, and `i32 -1` is `0xFFFFFFFF`.
- **Arithmetic is the same code that folds constants** (`fold::binary`/`fold::unary`, and `fold::convert` for `CAST`), so a folded answer and a computed one cannot drift apart.
- **A global initializer is a frame of its own** — the IR gives it no `frame_size`, so the machine gives it the cells its vregs name, and it runs before the entrypoint exists.
- **The machine validates first.** Every operand is read through an index, so a program the validator rejects never reaches the cell array; it comes back as a list of errors instead.
- **Limits are the machine's, not the IR's.** A frame's size and how deep recursion may go are not in this contract, so an executor has to say where it stops rather than asking the host for memory it may not have. The reference interpreter counts both the entrypoint's own frame and the initializers', and reports two kinds of stop: structure (a stack deeper than the limit, a frame wider than the whole cell array, a cell array with no room left — the heap shares that last limit) and pointers (a `LOAD`/`STORE`/`free` through an address that names no live cell). The pointer half is the direction this contract under-constrains: because a cell's address is the executor's encoding, the executor — not the IR — decides which addresses are live and reports the dead ones the same way it reports a divide.
- **What a run leaves behind is the observable result** and nothing else: the bytes the output primitives wrote to stdout (in order), final global state (one cell per global, in declaration order), and the exit code — the entrypoint's `i32` return, or the code `exit`/`abort` gave. A trap keeps the call stack it happened on, because the IR is nameless and the stack is the only way to say where a program was.
