#ifndef __LUAMAKE_PRE_IR_HPP
#define __LUAMAKE_PRE_IR_HPP

#include "luamake_strings.hpp"
#include <memory>
#include <string>

namespace luamake {
namespace ir {
struct Exception {
  virtual ~Exception() = default;
  virtual auto what() const noexcept -> std::string = 0;
};

struct Parse_Exc final : public Exception {
  ~Parse_Exc() final = default;
  auto what() const noexcept -> std::string final;
};

struct Interpret_Exc final : public Exception {
  ~Interpret_Exc() final = default;
  auto what() const noexcept -> std::string final;
};

// TODO: current the parser is an aggressive parser,
// see about how it performs changing to a lazy parser,
// this would require changing the API, basically just having an
// interpret function
struct IR final {
  /**
   * @throws std::bad_alloc
   */
  IR();
  IR(IR const &) = delete;
  IR &operator=(IR const &) = delete;
  IR(IR &&) = default;
  IR &operator=(IR &&) = default;
  ~IR() = default;

  enum types {
    IF,
    IFDEF,
    IFNDEF,
    ELIF,
    ELSE,
    ENDIF,
    GLOBAL_INCLUDE,
    LOCAL_INCLUDE,
    DEFINE,
  };
  size_t size;
  size_t cap;
  std::unique_ptr<types[]> types;
  std::unique_ptr<std::string[]> exprs;
  /**
   * @throws Parse_Exc <: Exception
   */
  static auto parse(FixedString const &) -> IR;

  /**
   * @throws Interpret_Exc <: Exception
   * TODO: figure out return type
   */
  auto interpret() -> void;

  auto check_size() -> void;
  // auto push(enum types &&t, std::string_view const) -> void;
  auto push(enum types &&t, std::string_view &&) -> void;
};
} // namespace ir
} // namespace luamake

#endif
