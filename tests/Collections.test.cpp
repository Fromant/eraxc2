#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "StdSource.hpp"
#include "frontend/parser/ProgramParser.hpp"
#include "frontend/tokenizer/Tokenizer.hpp"
#include "interp/Interpreter.hpp"

namespace {
  erax::frontend::ProgramParser::result parse_collections(std::string_view source) {
    auto imported = erax::frontend::Tokenizer {}.tokenize(erax::std_source, "std.erx");
    auto input = erax::frontend::Tokenizer {}.tokenize(source, "collections.erx");
    REQUIRE(imported.has_value());
    REQUIRE(input.has_value());
    std::vector<erax::frontend::Token> tokens;
    tokens.insert(tokens.end(), imported->begin(), std::prev(imported->end()));
    tokens.insert(tokens.end(), input->begin(), input->end());
    return erax::frontend::ProgramParser {tokens}.parse();
  }

  erax::interp::Outcome run_collections(std::string_view source, const erax::interp::Limits& limits = {}) {
    auto program = parse_collections(source);
    INFO((program ? std::string {} : program.error().message));
    REQUIRE(program.has_value());
    auto result = erax::interp::run(*program, limits);
    INFO((result ? std::string {} : erax::interp::to_string(result.error())));
    REQUIRE(result.has_value());
    return *result;
  }

  const std::string resource = R"(
    let drops: i32 = 0;
    let total: i32 = 0;
    struct Resource {
      id: i32;
      public new(id: i32): Resource { return { id: id }; }
      public fn number(): i32 { return this.id; }
      destructor { drops += 1; total += this.id; }
    }
  )";
}

TEST_CASE("the shipped hello world example prints a string through std") {
  std::ifstream input {std::string(ERAX_EXAMPLES_DIR) + "/helloworld.erx", std::ios::binary};
  REQUIRE(input.good());
  std::ostringstream source;
  source << input.rdbuf();
  CHECK(run_collections(source.str()).output == "Hello, \"world\"!\nOn a new line");
}

TEST_CASE("std strings own mutable byte sequences with independent copies") {
  auto result = run_collections(R"erx(
    fn main(): i32 {
      let mut text = "ab";
      const copy = text.clone();
      text.set(0, 'A');
      text.append(ref text);
      std.assert(text.length() == 4 && text.pop() == 'b');
      std.assert(text.at(2) == 'A' && copy.at(0) == 'a');
      const part = text.slice(1, 3);
      const joined = part.concat(ref copy);
      let mut expected = std.string.new();
      expected.reserve(4);
      expected.append(ref part);
      expected.append(ref copy);
      std.assert(joined.equals(ref expected) && !joined.equals(ref copy));
      text.clear();
      std.assert(text.empty() && "".empty() && text.slice(0, 0).empty());
      std.print(ref joined);
      std.print("!");
      return 0;
    }
  )erx");
  CHECK(result.code == 0);
  CHECK(result.output == "bAab!");
}

TEST_CASE("string literals preserve zero bytes and encode Unicode escapes as UTF-8") {
  auto result = run_collections(R"erx(
    fn main(): i32 {
      const text = "A\0\xFF\u00E9\u20AC";
      std.assert(text.length() == 8);
      text.write();
      return 0;
    }
  )erx");
  CHECK(result.code == 0);
  CHECK(result.output == std::string("A\0\xff\xc3\xa9\xe2\x82\xac", 8));

  auto malformed = parse_collections(R"erx(fn main(): i32 { std.print("\x1!"); return 0; })erx");
  REQUIRE_FALSE(malformed.has_value());
  CHECK(malformed.error().message == "expected 2 hexadecimal digits after \\x");
}

TEST_CASE("std string bounds failures use the assertion abort contract") {
  CHECK(run_collections(R"(fn main(): i32 { "a".at(1); return 0; })").code == 134);
  CHECK(run_collections(R"(fn main(): i32 { let mut s = ""; s.set(0, 'a'); return 0; })").code == 134);
  CHECK(run_collections(R"(fn main(): i32 { "a".slice(1, 0); return 0; })").code == 134);
}

TEST_CASE("the shipped RAII example is a collection conformance program") {
  std::ifstream input {std::string(ERAX_EXAMPLES_DIR) + "/raii.erx", std::ios::binary};
  REQUIRE(input.good());
  std::ostringstream source;
  source << input.rdbuf();
  const auto result = run_collections(source.str());
  CHECK(result.code == 0);
  CHECK(result.globals == std::vector<erax::interp::Cell> {2, 8});
}

TEST_CASE("nested generic closers do not become new expression operators") {
  CHECK(run_collections(R"(
    fn main(): i32 {
      const values = std.vector<std.unique<std.vector<i32>>>.new();
      return values.length() as i32;
    }
  )")
            .code == 0);
  auto invalid = parse_collections("fn main(): i32 { return 1 >>> 2; }");
  REQUIRE_FALSE(invalid.has_value());
  CHECK(invalid.error().message == "unknown operator '>>>'");
}

TEST_CASE("structure capability bounds validate the concrete cloning implementation") {
  auto invalid = parse_collections(R"(
    struct NeedsCopy<T extends Clonable> {
      public new(): NeedsCopy<T> { return {}; }
    }
    fn main(): i32 {
      const value = NeedsCopy<std.vector<std.unique<i32>>>.new();
      return 0;
    }
  )");
  REQUIRE_FALSE(invalid.has_value());
  CHECK(invalid.error().message.find("Clonable") != std::string::npos);
}

TEST_CASE("std unique destroys an owned user resource exactly once") {
  auto outcome = run_collections(resource + R"(
    fn main(): i32 {
      {
        const owner = std.unique<Resource>.new(Resource.new(7));
        std.assert(owner.get().number() == 7);
      }
      return drops;
    }
  )");
  CHECK(outcome.code == 1);
  REQUIRE(outcome.globals.size() == 2);
  CHECK(outcome.globals[0] == 1);
  CHECK(outcome.globals[1] == 7);
}

TEST_CASE("std unique get inherits the receiver access") {
  const auto outcome = run_collections(R"(
    fn main(): i32 {
      const mut owner = std.unique<i32>.new(7);
      *owner.get() = 9;
      return *owner.get();
    }
  )");
  CHECK(outcome.code == 9);
  const auto program = parse_collections(R"(
    fn main(): i32 {
      const owner = std.unique<i32>.new(7);
      *owner.get() = 9;
      return 0;
    }
  )");
  REQUIRE_FALSE(program.has_value());
}

TEST_CASE("std unique release transfers ownership and leaves an empty owner") {
  auto outcome = run_collections(resource + R"(
    fn main(): i32 {
      {
        const mut owner = std.unique<Resource>.new(Resource.new(7));
        { const value = owner.release(); std.assert(value.number() == 7); }
        std.assert(drops == 1);
      }
      return drops;
    }
  )");
  CHECK(outcome.code == 1);
  CHECK(outcome.globals[1] == 7);
}

TEST_CASE("std vector cloning creates independent scalar storage") {
  const auto outcome = run_collections(R"(
    fn main(): i32 {
      const mut values = std.vector<i32>.new();
      values.push(7);
      const mut copied = values;
      values.push(9);
      *copied.at(0) = 42;
      std.assert(values.length() == 2);
      std.assert(copied.length() == 1);
      std.assert(*values.at(0) == 7);
      std.assert(*copied.at(0) == 42);
      std.assert(copied.pop() == 42);
      std.assert(copied.empty());
      return 0;
    }
  )");
  CHECK(outcome.code == 0);
}

TEST_CASE("std vector of unique resources survives relocation and return by move") {
  const auto outcome = run_collections(resource + R"(
    fn make(): std.vector<std.unique<Resource>> {
      const mut values = std.vector<std.unique<Resource>>.new();
      values.push(std.unique<Resource>.new(Resource.new(3)));
      values.push(std.unique<Resource>.new(Resource.new(5)));
      values.reserve(20);
      std.assert(drops == 0);
      return move values;
    }
    fn consume(values: std.vector<std.unique<Resource>>) {
      std.assert(values.length() == 2);
    }
    fn main(): i32 {
      consume(make());
      std.assert(drops == 2);
      std.assert(total == 8);
      return 0;
    }
  )");
  CHECK(outcome.code == 0);
  REQUIRE(outcome.globals.size() == 2);
  CHECK(outcome.globals[0] == 2);
  CHECK(outcome.globals[1] == 8);
}

TEST_CASE("std vector clear and pop transfer or destroy each initialized element once") {
  const auto outcome = run_collections(resource + R"(
    fn main(): i32 {
      {
        const mut values = std.vector<std.unique<Resource>>.new();
        values.push(std.unique<Resource>.new(Resource.new(3)));
        values.push(std.unique<Resource>.new(Resource.new(5)));
        { const owner = values.pop(); }
        std.assert(drops == 1);
        values.clear();
        std.assert(drops == 2);
        values.clear();
      }
      return drops;
    }
  )");
  CHECK(outcome.code == 2);
  CHECK(outcome.globals[1] == 8);
}

TEST_CASE("std owning collections recycle resource storage across repeated scopes") {
  erax::interp::Limits limits;
  limits.max_cells = 1024;
  const auto outcome = run_collections(resource + R"(
    fn main(): i32 {
      for (let i = 0; i < 200; i += 1) {
        const mut values = std.vector<std.unique<Resource>>.new();
        values.push(std.unique<Resource>.new(Resource.new(3)));
        values.push(std.unique<Resource>.new(Resource.new(5)));
        values.reserve(20);
      }
      return drops;
    }
  )",
                                       limits);
  CHECK(outcome.code == 400);
  CHECK(outcome.globals[1] == 1600);
}

TEST_CASE("std owning containers do not copy nonclonable elements") {
  auto program = parse_collections(resource + R"(
    fn main(): i32 {
      const owner = std.unique<Resource>.new(Resource.new(7));
      const copied = owner;
      return 0;
    }
  )");
  REQUIRE_FALSE(program.has_value());
  CHECK(program.error().message.find("Clonable") != std::string::npos);
  program = parse_collections(resource + R"(
    fn main(): i32 {
      const values = std.vector<std.unique<Resource>>.new();
      const copied = values;
      return 0;
    }
  )");
  REQUIRE_FALSE(program.has_value());
  CHECK(program.error().message.find("Clonable") != std::string::npos);
}

TEST_CASE("std vector element references cannot survive storage invalidation") {
  for (const auto operation : {"values.reserve(20);", "values.clear();", "values.pop();"}) {
    INFO(operation);
    auto program = parse_collections(std::string(R"(
      fn main(): i32 {
        const mut values = std.vector<i32>.new();
        values.push(7);
        const item = values.at(0);
    )") + operation + "return *item; }");
    REQUIRE_FALSE(program.has_value());
    INFO(program.error().message);
    CHECK(program.error().message.find("reference") != std::string::npos);
  }
}

TEST_CASE("std vector object references remain valid across internal buffer changes") {
  const auto outcome = run_collections(R"(
    fn main(): i32 {
      const mut values = std.vector<i32>.new();
      const whole = ref values;
      values.push(7);
      values.reserve(20);
      return whole.length() as i32;
    }
  )");
  CHECK(outcome.code == 1);
}

TEST_CASE("std element references observe ordinary mutation through another valid alias") {
  const auto outcome = run_collections(R"(
    fn main(): i32 {
      const mut values = std.vector<i32>.new();
      values.push(7);
      const reading = values.at(0);
      *values.at(0) = 9;
      return *reading;
    }
  )");
  CHECK(outcome.code == 9);
}

TEST_CASE("std collection bounds failures use the assertion abort contract") {
  CHECK(run_collections("fn main(): i32 { const values = std.vector<i32>.new(); values.at(0); return 0; }").code ==
        134);
  CHECK(run_collections("fn main(): i32 { const mut values = std.vector<i32>.new(); values.pop(); return 0; }").code ==
        134);
}

TEST_CASE("fallible factories transfer success and clean up ordinary error returns") {
  const auto result = run_collections(R"(
    let drops: i32 = 0;
    struct Item {
      value: i32;
      public new(n: i32): std.Result<Item, i32> {
        const prepared = Item.prepare();
        if (n < 0) { return std.Result<Item, i32>.err(n); }
        return std.Result<Item, i32>.ok({ value: n });
      }
      new(): Item { return { value: 0 }; }
      static fn prepare(): Item { return Item.new(); }
      public fn get(): i32 { return this.value; }
      destructor { drops += 1; }
    }
    fn main(): i32 {
      const mut failed = Item.new(-3);
      if (failed.is_ok() || drops != 1) { return 1; }
      if (failed.take_error() != -3) { return 2; }
      {
        const mut success = Item.new(7);
        if (!success.is_ok()) { return 3; }
        const item = success.take_value();
        if (item.get() != 7) { return 4; }
      }
      { const ignored = Item.new(8); }
      return drops - 5;
    }
  )");
  CHECK(result.code == 0);
  CHECK(result.globals[0] == 5);
}
