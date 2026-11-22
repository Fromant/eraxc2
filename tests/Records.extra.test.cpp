#include "ParserTest.hpp"
#include "interp/Interpreter.hpp"

using namespace erax::test;

namespace {
  erax::interp::Outcome run_extra_record(std::string_view source) {
    const auto program = parse_ok(source);
    auto result = erax::interp::run(program);
    INFO((result ? "" : erax::interp::to_string(result.error())));
    REQUIRE(result.has_value());
    return *result;
  }
}

TEST_CASE("a signature can instantiate a structure from a later namespace") {
  CHECK(run_extra_record(R"(
    fn read(value: ref later.Box<i32>): i32 { return *value.get(); }
    namespace later {
      struct Box<T> {
        value: T;
        public new(value: T): Box<T> { return { value: move value }; }
        public fn get(this: ref Box<T>): ref T { return ref this.value; }
      }
    }
    fn main(): i32 { const box = later.Box<i32>.new(7); return read(ref box); }
  )")
            .code == 7);
}

TEST_CASE("owned structure fields cannot recursively contain themselves") {
  CHECK(parse_error(R"(
    struct A { value: B; }
    struct B { value: A; }
    fn main(): i32 { return 0; }
  )")
            .find("cycle") != std::string::npos);
  parse_ok(R"(
    struct A { next: ptr A; public new(): A { return { next: 0 as ptr A }; } }
    fn main(): i32 { const a = A.new(); return 0; }
  )");
}

TEST_CASE("replacing an owned field invalidates references to its old interior") {
  CHECK(parse_error(R"(
    struct Child {
      public value: i32;
      public new(value: i32): Child { return { value: value }; }
    }
    struct Holder {
      public child: Child;
      public new(): Holder { return { child: Child.new(1) }; }
    }
    fn main(): i32 {
      const mut holder = Holder.new();
      const old = ref holder.child.value;
      holder.child = Child.new(2);
      return *old;
    }
  )")
            .find("invalidated") != std::string::npos);
}

TEST_CASE("method invalidation follows explicit delegated mutable access") {
  CHECK(parse_error(R"(
    struct Child {
      public value: i32;
      public new(value: i32): Child { return { value: value }; }
    }
    fn replace(child: ref mut Child) { *child = Child.new(2); }
    struct Holder {
      public child: Child;
      public new(): Holder { return { child: Child.new(1) }; }
      public fn reset(this: ref mut Holder) { replace(ref mut this.child); }
    }
    fn main(): i32 {
      const mut holder = Holder.new();
      const old = ref holder.child.value;
      holder.reset();
      return *old;
    }
  )")
            .find("invalidated") != std::string::npos);
}

TEST_CASE("a scalar setter does not inherit unrelated resource invalidation") {
  CHECK(run_extra_record(R"(
    struct Value {
      public number: i32;
      storage: ptr u8;
      public new(): Value { return { number: 1, storage: primitive alloc(8) }; }
      public fn increment(this: ref mut Value) { this.number += 1; }
      public fn release(this: ref mut Value) {
        primitive free(this.storage);
        this.storage = 0 as ptr u8;
      }
      destructor { primitive free(this.storage); }
    }
    fn main(): i32 {
      const mut value = Value.new();
      const number = ref value.number;
      value.increment();
      return *number;
    }
  )")
            .code == 2);
}

TEST_CASE("compound assignment captures its original value before RHS calls") {
  CHECK(run_extra_record(R"(
    let value: i32 = 2;
    fn change(): i32 { value = 9; return 1; }
    fn main(): i32 { value += change(); return value; }
  )")
            .code == 3);
  CHECK(run_extra_record(R"(
    let value: i32 = 2;
    fn change(): i32 { value = 9; return 1; }
    fn main(): i32 { const pointer = &value; *pointer += change(); return value; }
  )")
            .code == 3);
  CHECK(run_extra_record(R"(
    fn change(value: ref mut i32): i32 { *value = 9; return 1; }
    fn main(): i32 { let mut value = 2; value += change(ref mut value); return value; }
  )")
            .code == 3);
}

TEST_CASE("free function summaries preserve harmless mutation and propagate invalidation") {
  CHECK(run_extra_record(R"(
    struct Point { public x: i32; public new(): Point { return { x: 1 }; } }
    fn increment(point: ref mut Point) { point.x += 1; }
    fn delegate(point: ref mut Point) { increment(point); }
    fn main(): i32 { const mut point = Point.new(); const x = ref point.x; delegate(ref mut point); return *x; }
  )")
            .code == 2);
  CHECK(parse_error(R"(
    struct Point { public x: i32; public new(): Point { return { x: 1 }; } }
    fn replace(point: ref mut Point) { *point = Point.new(); }
    fn delegate(point: ref mut Point) { replace(point); }
    fn main(): i32 { const mut point = Point.new(); const x = ref point.x; delegate(ref mut point); return *x; }
  )")
            .find("invalidated") != std::string::npos);
}

TEST_CASE("readonly references cannot recover mutable access by copying a nested reference") {
  CHECK(parse_error(R"(
    fn main(): i32 {
      let mut value = 1;
      const writer = ref mut value;
      const readonly = ref writer;
      const extracted = *readonly;
      *extracted = 9;
      return value;
    }
  )") != "");
  CHECK(parse_error(R"(
    fn main(): i32 {
      let mut value = 1;
      const writer = ref mut value;
      const pointer_to_writer = ref mut writer;
      return value;
    }
  )")
            .find("mutable access") != std::string::npos);
}
