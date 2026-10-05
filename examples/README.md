# Examples

These small programs show what erax looks like today (v1). They only use features the parser/JIR implement, so they lower cleanly with `--jir` and run with `--run`.

- `entry.erax` -- The entrypoint returns an exit code (`i32`). No globals.
- `globals.erax` -- A global initialized by a constant `0`; `1` and `2` are added/multiplied in the function.
- `sum_to_ten.erax` -- A `for` loop: the test is a real branch, so the JIR gets `CMP_LT` and four blocks.
- `fib.erax` -- A recursive function. `CALL dst &f a0…` has `args[0]` = result slot, `args[1]` = callee, `args[2..]` = arguments.
- `folded_test.erax` -- A `while (true)` whose test is constant is still a loop; only the `if (i > 3)` branch remains.
- `types.erax` -- All types in one place (`i8`–`u64`, `bool`, `char`). The boundary values show why the two widest need a suffix: `i64min` is written as `-9223372036854775807i64 - 1i64` and `u64max` as `18446744073709551615u64`, because a bare literal is an `i32` unless the position says otherwise (`types.md#literals`).
- `helloworld.erax` -- An aspirational example that uses `string` and `std.print`. Those are not in v1, so it is expected to fail lowering at the moment.

To see the lowered JIR for any of these:

```powershell
.\build\eraxc.exe --jir examples\sum_to_ten.erax
```

To run one: `--run` exits with the entrypoint's return value, which is what makes these the conformance corpus (`tests/Interpreter.test.cpp` runs the same files and checks the same numbers).

```powershell
.\build\eraxc.exe --run examples\fib.erax    # prints nothing, exits 55
```
