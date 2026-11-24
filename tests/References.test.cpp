#include "ParserTest.hpp"
#include "interp/Interpreter.hpp"

using namespace erax::test;

namespace {
  std::uint64_t reference_code(std::string_view source) {
    const auto program = parse_ok(source);
    const auto outcome = erax::interp::run(program);
    if (!outcome) {
      INFO(erax::interp::to_string(outcome.error()));
    }
    REQUIRE(outcome.has_value());
    return outcome->code;
  }
}

TEST_CASE("safe references preserve explicit mutation permissions and allow aliases") {
  CHECK(reference_code("fn main(): i32 { const mut value = 7; const first = ref mut value; "
                       "const second = first; *first = 10; *second += 1; return value; }") == 11);
  CHECK(reference_code("fn main(): i32 { let value = 7; const reader = ref value; value = 9; return *reader; }") == 9);
  CHECK(parse_error("fn main(): i32 { const value = 7; const writer = ref mut value; return 0; }") ==
        "ref mut needs mutable access");
  CHECK(parse_error("fn main(): i32 { let mut value = 7; const reader = ref value; *reader = 9; return value; }") !=
        "");
  CHECK(parse_error("fn main(): i32 { const reader = ref 7; return 0; }") == "ref needs an existing storage place");
}

TEST_CASE("reference return dependencies distinguish caller objects from callee storage") {
  CHECK(reference_code("fn identity(value: ref i32): ref i32 { return value; } "
                       "fn main(): i32 { const value = 7; const reader = identity(ref value); return *reader; }") == 7);
  CHECK(reference_code("fn read(value: ref i32): i32 { return *value; } "
                       "fn main(): i32 { const value = 7; return read(ref value); }") == 7);
  CHECK(parse_error("fn bad(): ref i32 { const value = 7; return ref value; } "
                    "fn main(): i32 { return 0; }") == "cannot return a value that references a local object");
  CHECK(parse_error("fn bad(value: i32): ref i32 { return ref value; } "
                    "fn main(): i32 { return 0; }") == "cannot return a value that references a local object");
}

TEST_CASE("reference fields preserve contained dependencies when their owner moves") {
  constexpr std::string_view view = R"(
    struct View {
      public target: ref i32;
      public new(target: ref i32): View { return { target: target }; }
      public fn get(): ref i32 { return this.target; }
    }
  )";
  CHECK(reference_code(std::string(view) +
                       "fn main(): i32 { const value = 7; let view = View.new(ref value); "
                       "const moved = move view; return *moved.get(); }") == 7);
  CHECK(reference_code(
            std::string(view) +
            "fn pass(view: View): View { return move view; } "
            "fn main(): i32 { const value = 7; const view = pass(View.new(ref value)); return *view.target; }") == 7);
  CHECK(parse_error(std::string(view) +
                    "fn bad(): View { const value = 7; return View.new(ref value); } "
                    "fn main(): i32 { return 0; }") == "cannot return a value that references a local object");
}

TEST_CASE("a safe reference cannot outlive a nested scope or a moved source") {
  CHECK(
      parse_error("fn main(): i32 { let reader: ref i32; { const value = 7; reader = ref value; } return *reader; }") ==
      "reference is used after its target was invalidated");
  CHECK(parse_error("fn main(): i32 { const value = 7; const reader = ref value; const moved = move value; "
                    "return *reader; }") == "reference is used after its target was invalidated");
  CHECK(reference_code("fn main(): i32 { const value = 7; const reader = ref value; const copy = *reader; "
                       "const moved = move value; return copy + moved; }") == 14);
}

TEST_CASE("reference invalidation follows CFG back edges and permits fresh iteration borrows") {
  CHECK(parse_error("fn main(): i32 { let value = 7; const reader = ref value; "
                    "for (let i = 0; i < 2; i += 1) { const copy = *reader; move value; value = copy; } return 0; }") ==
        "reference may be invalidated on a control-flow path");
  CHECK(reference_code("fn main(): i32 { let value = 7; "
                       "for (let i = 0; i < 2; i += 1) { const reader = ref value; const copy = *reader; "
                       "move value; value = copy; } return value; }") == 7);
  CHECK(reference_code("fn main(): i32 { let value = 7; const reader = ref value; if (false) { move value; } "
                       "return *reader; }") == 7);
}

TEST_CASE("immutable owners cannot leak mutable access through reference fields") {
  constexpr std::string_view holder = R"(
    struct Holder {
      public target: ref mut i32;
      public new(target: ref mut i32): Holder { return { target: target }; }
    }
  )";
  CHECK(parse_error(std::string(holder) +
                    "fn main(): i32 { let mut value = 7; const holder = Holder.new(ref mut value); "
                    "const extracted = holder.target; *extracted = 9; return value; }") != "");
  CHECK(reference_code(std::string(holder) +
                       "fn main(): i32 { let mut value = 7; const mut holder = Holder.new(ref mut value); "
                       "const extracted = holder.target; *extracted = 9; return value; }") == 9);
}

TEST_CASE("raw pointer casts cannot manufacture a safe reference") {
  CHECK(parse_error("fn main(): i32 { let value = 7; const reader = &value as ref i32; return 0; }") != "");
  CHECK(parse_error("fn main(): i32 { const reader = 0 as ref i32; return 0; }") != "");
}

TEST_CASE("global reference storage is rejected without weakening local reference safety") {
  CHECK(reference_code(R"(
    namespace counters { let mut value: i32 = 1; }
    fn borrow(): ref mut i32 { return ref mut counters.value; }
    fn main(): i32 {
      const alias = borrow();
      *alias = 9;
      return counters.value;
    }
  )") == 9);
  CHECK(parse_error(R"(
    struct Resource {
      public value: i32;
      public new(): Resource { return { value: 1 }; }
      public fn get(): ref i32 { return ref this.value; }
    }
    const value: Resource = Resource.new();
    fn main(): i32 { const reader = value.get(); return *reader; }
  )")
            .find("global") != std::string::npos);
  CHECK(parse_error(R"(
    struct View {
      target: ref i32;
      public new(target: ref i32): View { return { target: target }; }
    }
    let value: i32 = 1;
    const view: View = View.new(ref value);
    fn main(): i32 { return 0; }
  )")
            .find("global") != std::string::npos);
}

TEST_CASE("writing reference fields preserves dependencies through mutable aliases and calls") {
  constexpr std::string_view holder = R"(
    struct Holder {
      public target: ref i32;
      public new(target: ref i32): Holder { return { target: target }; }
      public mut fn set(target: ref i32) { this.target = target; }
    }
  )";
  CHECK(parse_error(std::string(holder) +
                    "fn main(): i32 { const old = 7; const mut holder = Holder.new(ref old); "
                    "{ const inner = 9; holder.target = ref inner; } return *holder.target; }") ==
        "reference is used after its target was invalidated");
  CHECK(parse_error(std::string(holder) +
                    "fn main(): i32 { const old = 7; const mut holder = Holder.new(ref old); "
                    "{ const inner = 9; holder.set(ref inner); } return *holder.target; }") ==
        "reference is used after its target was invalidated");
  CHECK(parse_error(std::string(holder) +
                    "fn bad(holder: ref mut Holder) { const inner = 9; holder.target = ref inner; } "
                    "fn main(): i32 { return 0; }") ==
        "cannot store a reference to a local object in an external object");
}

TEST_CASE("implicit destruction checks the lifetime of a record's borrowed fields") {
  CHECK(parse_error(R"(
    struct Reader {
      public target: ref i32;
      public new(target: ref i32): Reader { return { target: target }; }
      destructor { primitive print(*this.target); }
    }
    fn main(): i32 {
      const old = 7;
      const mut reader = Reader.new(ref old);
      { const inner = 9; reader.target = ref inner; }
      return 0;
    }
  )") == "reference is used after its target was invalidated");
}

TEST_CASE("custom generic copying depends on its body rather than every type argument") {
  CHECK(reference_code(R"(
    struct Noncopyable {
      value: i32;
      public new(value: i32): Noncopyable { return { value: value }; }
      destructor {}
    }
    struct Phantom<T> {
      public value: i32;
      public new(value: i32): Phantom<T> { return { value: value }; }
      public fn clone(): Phantom<T> { return Phantom<T>.new(this.value); }
    }
    fn duplicate<T extends Clonable>(value: ref T): T { return *value; }
    fn main(): i32 {
      const original = Phantom<Noncopyable>.new(7);
      const copied = original;
      const generic_copy = duplicate(ref copied);
      return generic_copy.value;
    }
  )") == 7);
}

TEST_CASE("reference-bearing custom copies retain their source lifetime dependency") {
  constexpr std::string_view source = R"(
    struct ReferenceCopy {
      own: i32;
      target: ref i32;
      public new(target: ref i32): ReferenceCopy { return { own: 9, target: target }; }
      public fn clone(): ReferenceCopy {
        return ReferenceCopy.new(ref this.own);
      }
    }
  )";
  CHECK(parse_error(std::string(source) +
                    "fn bad(external: ref i32): ReferenceCopy { const local = ReferenceCopy.new(external); "
                    "return local; } fn main(): i32 { return 0; }") ==
        "cannot return a value that references a local object");
  CHECK(parse_error(std::string(source) +
                    "fn bad(external: ref i32): ReferenceCopy { const local = ReferenceCopy.new(external); "
                    "return local.clone(); } fn main(): i32 { return 0; }") ==
        "cannot return a value that references a local object");
}

TEST_CASE("conditional method results inherit receiver access without granting mutation to the body") {
  const std::string declaration = R"(
    struct Entry {
      key: i32;
      value: i32;
      public new(): Entry { return { key: 1, value: 2 }; }
      public fn key_ref(): ref i32 { return ref this.key; }
      public fn value_ref(): ref mut? i32 { return ref mut? this.value; }
    }
  )";
  CHECK(reference_code(declaration + R"(
    fn main(): i32 {
      const mut item = Entry.new();
      *item.value_ref() = 9;
      const reader = ref item;
      const value: ref i32 = reader.value_ref();
      return *value;
    }
  )") == 9);
  CHECK(parse_error(declaration + R"(
    fn main(): i32 { const mut item = Entry.new(); *item.key_ref() = 9; return 0; }
  )") != "");
  CHECK(parse_error(R"(
    struct Entry {
      value: i32;
      public fn bad(): ref mut? i32 { this.value = 9; return ref mut? this.value; }
    }
    fn main(): i32 { return 0; }
  )") != "");
  CHECK(parse_error(R"(
    struct Entry {
      public fn bad(other: ref i32): ref mut? i32 { return ref mut? *other; }
    }
    fn main(): i32 { return 0; }
  )") == "ref mut? requires a place projected through this");
}
