file(MAKE_DIRECTORY "${WORK}")
file(WRITE "${WORK}/common.erx" [=[
export let counter: i32 = 4;
fn offset(): i32 { return 10; }
export fn add<T extends Arithmetic>(x: T): T { return x + (offset() as T); }
fn add(x: i32): i32 { return x + counter; }
export namespace tools {
  export namespace inner {
    export struct Value {
      n: i32;
      public new(n: i32): Value { return { n: n }; }
      public fn get(): i32 { return this.n; }
    }
    struct Hidden {}
  }
  fn secret(): i32 { return 12; }
}
export fn main(x: i32): i32 { return x; }
]=])
file(WRITE "${WORK}/left.erx" [=[
import { counter, add, tools } from "./common.erx";
export const initial: i32 = counter + 1;
export fn step(): tools.inner.Value {
  counter += 1;
  return tools.inner.Value.new(add(counter));
}
]=])
file(WRITE "${WORK}/right.erx" [=[
import { counter, add } from "./common.erx";
export fn read(): i32 { return counter + (add(1i64) as i32); }
]=])

function(run source expected_error)
    file(WRITE "${WORK}/main.erx" "${source}")
    execute_process(COMMAND "${ERAXC}" "main.erx" WORKING_DIRECTORY "${WORK}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(expected_error STREQUAL "")
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Expected success, got ${result}\n${output}${error}")
        endif()
    elseif(result EQUAL 0 OR NOT error MATCHES "${expected_error}")
        message(FATAL_ERROR "Expected '${expected_error}', got ${result}\n${output}${error}")
    endif()
endfunction()

run([=[
import { step, initial } from "./left.erx";
import { read } from "./right.erx";
import { tools as t, main as echo, } from "./common.erx";
fn offset(): i32 { return 99; }
namespace app {
  fn check(): bool {
    const item: t.inner.Value = step();
    return item.get() == 10 && initial == 5 && read() == 16 && echo(0) == 0;
  }
}
fn main(): i32 { if (app.check()) { return 0; } return 9; }
]=] "")
run([=[
import { offset } from "./common.erx";
fn main(): i32 { return 0; }
]=] "module does not export 'offset'")
run([=[
import { add } from "./left.erx";
fn main(): i32 { return 0; }
]=] "module does not export 'add'")
run([=[
fn main(): i32 { return 0; }
import { add } from "./common.erx";
]=] "imports must precede declarations")
run([=[
import { tools } from "./common.erx";
fn main(): i32 { return tools.secret(); }
]=] "undeclared function 'tools.secret'")
run([=[
import { tools } from "./common.erx";
fn main(): i32 { let x: tools.inner.Hidden; return 0; }
]=] "expected a type")
run([=[
import { counter } from "./common.erx";
fn counter(): i32 { return 0; }
fn main(): i32 { return 0; }
]=] "conflicts with import at main.erx:1:")
run([=[
import { counter, add as counter } from "./common.erx";
fn main(): i32 { return 0; }
]=] "first imported at main.erx:1:")
run([=[
import { counter } from "./missing.erx";
fn main(): i32 { return 0; }
]=] "cannot open module '.*missing.erx'")
file(WRITE "${WORK}/cycle.erx" [=[
import { start } from "./main.erx";
export fn recurse(): i32 { return start(); }
]=])
run([=[
import { recurse } from "./cycle.erx";
export fn start(): i32 { return recurse(); }
fn main(): i32 { return 0; }
]=] "import cycle: .*main.erx -> .*cycle.erx -> .*main.erx")
if(WIN32 AND EXISTS "${WORK}/COMMON.erx")
    run([=[
import { counter as first } from "./common.erx";
import { counter as second } from "./COMMON.erx";
fn main(): i32 { first = 9; return second - 9; }
]=] "")
endif()
