# Middleend — JIR

JIR is the single IR of erax: what the parser emits and what every backend executes. It is a register-style, stack-framed IR — a program runs on one machine stack of cells, so a compliant executor is a stack machine by construction (an array of cells plus a frame pointer; nothing else is required).

Roadmap and format versioning live in `TODO.md`; this file is the language contract only.

## Grammar

```
program     = global* function* entrypoint
global      = type , optional init cfg          ; empty init = zero-initialized
function    = function id , return type , param types , frame size , cfg
entrypoint  = function id                       ; zero params, returns i32
command     = OP operand*                       ; args[0] is the result slot of ops that produce one
```

A function's **frame size** is the number of cells its frame occupies — one more than its highest vreg id. All control flow between blocks lives on CFG edges; there are no jump commands.

## Lexicon

Every operand is `(kind, type, value)` — self-describing; nothing is resolved at load or run time.

| Operand | Print        | Value means                                   |
| ------- | ------------ | --------------------------------------------- |
| Instant | `type value` (e.g. `i32 5`, `u8 1`) | the immediate                          |
| VReg    | `type $k` (e.g. `i32 $3`) | cell `fp + k` of that type in the current frame |
| Global  | `type @k` (e.g. `i32 @0`) | cell `k` of the process-wide globals array |
| Fn      | `&k`         | function id `k`                               |
| EMPTY   | `EMPTY`      | the empty sentinel — names "no cell" (see [calls](#calls)) |

- Every operand prints its type, so a printed instruction is self-describing: a cell's type comes from the
  instruction that names it, never from a separate table.
- A `VReg` id is an **offset within the current frame**, always `0 ≤ k < frame_size`.
- A `Fn` operand carries the callee's return type; a `Global` operand carries the global's type.
- An `Instant`'s value is the **raw bit pattern** of the cell it fills; its printed form is interpreted with the type's signedness (`i8 0xFF` prints as `i8 -1`).
- The `EMPTY` sentinel is `(VReg, void, u64(-1))` — a phantom offset that names "no cell". It is legal only as the result slot of a `CALL`.

## Types

| Type           | Bytes |
| -------------- | ----- |
| `void`         | 0     |
| `i8` `u8`      | 1     |
| `i16` `u16`    | 2     |
| `i32` `u32`    | 4     |
| `i64` `u64`    | 8     |

- `bool` lowers to `i8`; `char` lowers to `u8` — it is a `0–255` byte, so its comparisons and its `/ % << >>` are unsigned.
- No implicit conversions: where an op requires matching types they must match exactly (the validator enforces this).

## Frames

- VReg ids address the **current frame** only: `$k` is the cell at `fp + k`.
- A `CALL` pushes a frame of the callee's `frame_size` cells and copies the arguments into its leading slots `$0..$n-1`; a `RET` pops it. Cells are consequently **killed on return** — locals live only while their function is on the stack.
- Recursion is ordinary: each activation has its own cells, and the same id in a nested activation addresses a different cell. The stack cost of a path is the sum of the nested frames' sizes.
- The frame layout is static: `frame_size` is a compile-time constant per function, so every activation costs a known number of cells. A function that uses no vregs still has `frame_size` 1 — a frame is never empty.

## Commands and control flow

- One shape for every command: `OP arg0 arg1 …`. Roles come from the op table; an op that produces a result writes `args[0]` (a vreg, or for `MOV` also a global).
- A function body (and a global initializer) is a **CFG**: nodes are ordered lists of commands; node `0` is the entry. Every node has **0 to 2 outgoing edges**:
  - `0` — the block ends in `RET`;
  - `1` — unconditional;
  - `2` — a branch: one edge carries a `Condition`, the other is the fallthrough.
- A `Condition` is `(cmp, a, b)` with `cmp ∈ CMP_EQ…CMP_GE` — the same ops as the `CMP_*` commands, drawn from the one op table — e.g. "`x` is nonzero" is `(CMP_NE, x, 0)`. Execution follows the edge whose condition holds, else the fallthrough.
- There are **no jump commands**.

## Operations

### Data movement

| Op    | Shape      | Semantics                                                    |
| ----- | ---------- | ------------------------------------------------------------ |
| `MOV` | `MOV dst src` | `dst = src` (`src`: instant/vreg/global; `dst`: vreg/global). The only way to read or write a global. |

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

Logical negation is `CMP_NE i8 $dst i8 $a i8 0`.

### Control

| Op     | Shape                              | Semantics                                                  |
| ------ | ---------------------------------- | ---------------------------------------------------------- |
| `CALL` | `CALL dst &f a0 … an-1`            | call `f`, see [calls](#calls)                              |
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

## Runtime semantics

Every value is defined in this contract or the program is rejected by the validator — there is no undefined behavior.

- **Overflow wraps** (two's complement): `i64max + 1 = i64min`.
- **Divide-by-zero = process death**; `iMIN / -1` dies too.
- **Div/mod truncate toward zero**, signed and unsigned.
- **Shift counts wrap to the operand width**: `count & (width-1)` — `i8`/`u8` → 3 bits, `i16`/`u16` → 4, `i32`/`u32` → 5, `i64`/`u64` → 6.
- **Signedness**: signed for `i*`, unsigned for `u*` (comparisons, div/mod, `SAR`).
- **Bool**: `i8` 0/1; a condition is always a `(cmp, a, b)` pair with C's "nonzero" rule.
- **Determinism**: ordered IR, single-threaded. The same program has the same observable result (final global state + entrypoint return) on every backend.

## Validator

Runs on every `Program` before any backend sees it. A producer leaves the CFG canonical: it drops the blocks no edge can reach (the parser knows which source constructs cannot run) and never lets a path fall off the end of a function.

1. **CFG**: entry block is 0; every block reachable; every edge's `from`/`to` valid; 0–2 outgoing edges per block; a two-edge block has exactly one conditional edge; no outgoing edges iff the block ends in `RET`.
2. **Commands**: arity and operand roles match the op table; operand types are the explicit ones on each operand; ops requiring same-type operands get them; no implicit conversions.
3. **Frames**: every vreg id `< frame_size`; params are `$0..n-1` in order.
4. **Definitions**: every vreg read has a reaching definition from entry (params and globals count as defined at entry); every vreg has one consistent type across all reads and writes.
5. **Calls**: `args[1]` is a `Fn` operand naming a valid function; `args[2..]` count and types match the callee's params and are all defined; `args[0]` is the `EMPTY` sentinel iff the callee returns void — a sentinel anywhere else is an error, and a non-void `dst` matches the callee's return type.
6. **Program**: entrypoint exists, zero params, `i32` return.
7. **Init CFGs**: the same rules as function CFGs, zero params.

## Execution model (possible implementation)

- **Interpreter / VM**: one array of cells; a frame pointer (plus a stack pointer) walks frames — `CALL` pushes, `RET` pops. The engine is a stack machine and nothing else.
- **AoT**: frames map to native stack frames; vregs resolve to frame-pointer-relative addresses; instruction selection, frame layout, and register allocation are backend concerns.
- Serialized `.jir` format, magic, and versioning: see `TODO.md`.