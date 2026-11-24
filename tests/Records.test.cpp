#include "ParserTest.hpp"
#include "StdSource.hpp"
#include "interp/Interpreter.hpp"

using namespace erax::test;

namespace {
  auto compile_record(std::string_view source, bool with_std = false) {
    std::string text;
    if (with_std) {
      text = erax::std_source;
    }
    text += source;
    auto tokens = Tokenizer {}.tokenize(text, "record.erx");
    REQUIRE(tokens.has_value());
    return ProgramParser {*tokens}.parse();
  }

  erax::interp::Outcome run_record(std::string_view source, bool with_std = false) {
    auto program = compile_record(source, with_std);
    INFO((program ? "" : program.error().message));
    REQUIRE(program.has_value());
    auto result = erax::interp::run(*program);
    INFO((result ? "" : erax::interp::to_string(result.error())));
    REQUIRE(result.has_value());
    return *result;
  }

  std::string record_error(std::string_view source) {
    auto program = compile_record(source);
    REQUIRE_FALSE(program.has_value());
    return program.error().message;
  }
}

TEST_CASE("factories construct private fields and methods use this") {
  auto result = run_record(R"(
    struct Counter {
      value: i32;
      public new(value: i32): Counter { return { value: value }; }
      public fn get(): i32 { return this.value; }
      public mut fn increment() { this.value += 1; }
      destructor { primitive print(this.value); }
    }
    fn main(): i32 { const mut value = Counter.new(4); value.increment(); return value.get(); }
  )");
  CHECK(result.code == 5);
  CHECK(result.output == "5\n");
}

TEST_CASE("structure creation and access preserve visibility and mutability") {
  const std::string declaration = R"(
    struct Value {
      private hidden: i32;
      public exposed: i32;
      public new(): Value { return { hidden: 1, exposed: 2 }; }
      public mut fn set() { this.hidden = 3; }
    }
  )";
  CHECK(record_error(declaration + "fn main(): i32 { const value = Value.new(); return value.hidden; }")
            .find("private") != std::string::npos);
  CHECK(record_error(declaration + "fn main(): i32 { const value = Value.new(); value.exposed = 3; return 0; }")
            .find("mut") != std::string::npos);
  CHECK(record_error(declaration + "fn main(): i32 { const value = Value.new(); value.set(); return 0; }") != "");
  CHECK(record_error(declaration + "fn main(): i32 { const value: Value = { hidden: 1, exposed: 2 }; return 0; }")
            .find("factory") != std::string::npos);
  CHECK(run_record(declaration +
                   "fn main(): i32 { const mut value = Value.new(); value.exposed = 7; return value.exposed; }")
            .code == 7);
}

TEST_CASE("a destructor runs once after a move and before outer cleanup") {
  auto result = run_record(R"(
    struct Resource {
      value: i32;
      public new(value: i32): Resource { return { value: value }; }
      destructor { primitive print(this.value); }
    }
    fn relay(value: Resource): Resource { return move value; }
    fn main(): i32 {
      const outer = Resource.new(1);
      { const inner = Resource.new(2); const transferred = relay(move inner); }
      return 0;
    }
  )");
  CHECK(result.output == "2\n1\n");
}

TEST_CASE("RAII destroys fields without a user destructor on the containing structure") {
  auto result = run_record(R"(
    struct Resource {
      value: i32;
      public new(value: i32): Resource { return { value: value }; }
      destructor { primitive print(this.value); }
    }
    struct Pair {
      first: Resource;
      second: Resource;
      public new(): Pair { return { first: Resource.new(1), second: Resource.new(2) }; }
    }
    fn main(): i32 { const pair = Pair.new(); return 0; }
  )");
  CHECK(result.output == "2\n1\n");
}

TEST_CASE("record copies use clone and both owners destroy independently") {
  auto result = run_record(R"(
    struct Resource {
      public value: i32;
      public new(value: i32): Resource { return { value: value }; }
      public fn clone(): Resource { return Resource.new(this.value); }
      destructor { primitive print(this.value); }
    }
    fn main(): i32 {
      const original = Resource.new(1);
      const mut copied = original;
      copied.value = 2;
      return original.value;
    }
  )");
  CHECK(result.code == 1);
  CHECK(result.output == "2\n1\n");
}

TEST_CASE("destructor-bearing records cannot accidentally shallow-copy") {
  CHECK(record_error(R"(
    struct Resource {
      public new(): Resource { return {}; }
      destructor {}
    }
    fn main(): i32 { const a = Resource.new(); const b = a; return 0; }
  )")
            .find("Clonable") != std::string::npos);
}

TEST_CASE("local cleanup runs on continue break and return") {
  auto result = run_record(R"(
    struct Resource {
      value: i32;
      public new(value: i32): Resource { return { value: value }; }
      destructor { primitive print(this.value); }
    }
    fn main(): i32 {
      for (let i = 0; i < 3; i += 1) {
        const value = Resource.new(i);
        if (i == 0) { continue; }
        break;
      }
      const last = Resource.new(9);
      return 0;
    }
  )");
  CHECK(result.output == "0\n1\n9\n");
}

TEST_CASE("safe references allow ordinary mutation and reject immutable writes") {
  CHECK(run_record("fn main(): i32 { let mut value = 1; const r = ref mut value; *r = 2; return value; }").code == 2);
  CHECK(run_record("fn main(): i32 { let value = 1; const r = ref value; value = 2; return *r; }").code == 2);
  CHECK(record_error("fn main(): i32 { const value = 1; const r = ref mut value; return 0; }") != "");
  CHECK(record_error("fn main(): i32 { let value = 1; const r = ref value; *r = 2; return 0; }") != "");
  CHECK(record_error("fn main(): i32 { const r = ref 1; return 0; }") != "");
}

TEST_CASE("safe references cannot escape a destroyed local or a local reference field") {
  CHECK(record_error("fn bad(): ref i32 { let value = 1; return ref value; } fn main(): i32 { return 0; }")
            .find("local") != std::string::npos);
  CHECK(record_error(R"(
    struct View {
      value: ref i32;
      public new(value: ref i32): View { return { value: value }; }
    }
    fn bad(): View { let value = 1; return View.new(ref value); }
    fn main(): i32 { return 0; }
  )")
            .find("local") != std::string::npos);
}

TEST_CASE("generic structure fields and methods specialize their type arguments") {
  CHECK(run_record(R"(
    struct Box<T> {
      public value: T;
      public new(value: T): Box<T> { return { value: move value }; }
      public fn get(): ref T { return ref this.value; }
    }
    fn main(): i32 { const value = Box<i32>.new(7); const borrowed = value.get(); return *borrowed; }
  )")
            .code == 7);
}

TEST_CASE("global owners initialize in source order and destroy after main in reverse order") {
  auto result = run_record(R"(
    let drops: i32 = 0;
    struct Resource {
      value: i32;
      public new(value: i32): Resource { return { value: value }; }
      public fn get(): i32 { return this.value; }
      destructor { drops += 1; primitive print(this.value); }
    }
    const first: Resource = Resource.new(1);
    const second: Resource = Resource.new(first.get() + 1);
    fn main(): i32 { return drops; }
  )");
  CHECK(result.code == 0);
  CHECK(result.output == "2\n1\n");
  REQUIRE(result.globals.size() == 3);
  CHECK(result.globals[0] == 2);
  CHECK(result.globals[1] == 0);
  CHECK(result.globals[2] == 0);
}

TEST_CASE("immediate termination bypasses owner destruction") {
  auto result = run_record(R"(
    struct Resource {
      public new(): Resource { return {}; }
      destructor { primitive print(999); }
    }
    const global_owner: Resource = Resource.new();
    fn main(): i32 { const local_owner = Resource.new(); primitive exit(7); }
  )");
  CHECK(result.code == 7);
  CHECK(result.output.empty());
}

TEST_CASE("method overloads resolve arguments and receiver permissions together") {
  CHECK(run_record(R"(
    struct Value {
      public new(): Value { return {}; }
      public fn read(value: i32): i32 { return value; }
      public mut fn read(value: bool): i32 { if (value) { return 2; } return 3; }
    }
    fn main(): i32 { const mut value = Value.new(); return value.read(7) + value.read(true); }
  )")
            .code == 9);
}

TEST_CASE("call arguments snapshot scalar values before later argument effects") {
  CHECK(run_record(R"(
    fn change(value: ref mut i32): i32 { *value = 9; return 1; }
    fn combine(first: i32, second: i32): i32 { return first * 10 + second; }
    fn main(): i32 { let mut value = 2; return combine(value, change(ref mut value)); }
  )")
            .code == 21);
}
