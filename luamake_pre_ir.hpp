#ifndef __LUAMAKE_PRE_IR_HPP
#define __LUAMAKE_PRE_IR_HPP

#include "luamake_strings.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace luamake {
namespace ir {
struct Exception {
  explicit Exception(decltype(__LINE__) line, std::string &&message) noexcept
      : line(line), message(message) {}

  virtual ~Exception() = default;
  virtual auto what() const noexcept -> std::string = 0;

  decltype(__LINE__) line;
  std::string message;
};

struct Lex_Exc final : public Exception {
  explicit Lex_Exc(decltype(__LINE__) line, std::string &&message) noexcept
      : Exception(line, std::move(message)) {}
  ~Lex_Exc() final = default;
  auto what() const noexcept -> std::string final;
};

struct Parse_Exc final : public Exception {
  explicit Parse_Exc(decltype(__LINE__) line, std::string &&message) noexcept
      : Exception(line, std::move(message)) {}

  ~Parse_Exc() final = default;
  auto what() const noexcept -> std::string final;
};

struct Interpret_Exc final : public Exception {
  explicit Interpret_Exc(decltype(__LINE__) line,
                         std::string &&message) noexcept
      : Exception(line, std::move(message)) {}

  ~Interpret_Exc() final = default;
  auto what() const noexcept -> std::string final;
};

struct Macro final {
  enum MacroType {
    INT,
    FLOAT,
    STRING,
    TYPE,
    ATTRIBUTE,
  };

  MacroType t;
  std::string m;
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
  /**
   * @throws Parse_Exc <: Exception
   */
  [[nodiscard]]
  static auto parse(FixedString const &) -> IR;

  /**
   * @throws Interpret_Exc <: Exception
   * TODO: figure out return type
   */
  [[nodiscard]]
  auto interpret(std::unordered_map<std::string, Macro> &,
                 std::unordered_set<std::string> &)
      -> std::vector<std::filesystem::path>;

private:
  auto check_size() -> void;
  // auto push(enum types &&t, std::string_view const) -> void;
  auto push(enum types &&t, std::string_view &&) -> void;

  auto constexpr pretty_types(enum types) -> char const *;

  struct Lexer final {
    enum types {
      // preprocessor stuff
      IF,
      IFDEF,
      IFNDEF,
      ELIF,
      ELSE,
      ENDIF,
      DEFINE,
      INCLUDE,
      // operators
      // TODO: add other operators
      LOG_AND,
      LOG_OR,
      OP_DEFINED,
      LPAREN,
      RPAREN,
      LANGLE,
      RANGLE,
      QUOTE,
      // values
      CHAR_LIT,
      INT_LIT,
      MACRO,
    };

    std::vector<Lexer::types> types;
    std::vector<std::string> lexemes;

    /**
     * @throws Lex_Exc
     */
    static auto lex(FixedString const &) -> Lexer;

    /**
     * @throws Lex_Exc
     */
    auto parse_to_ir() -> ir::IR;

    auto to_string(enum Lexer::types t) const -> std::string;
  };
  auto interpret_impl(size_t &, bool, std::unordered_map<std::string, Macro> &,
                      std::unordered_set<std::string> &,
                      std::vector<std::filesystem::path> &) -> void;

  size_t size;
  size_t cap;
  std::unique_ptr<types[]> types;
  std::unique_ptr<std::string[]> exprs;
  friend Lexer;
};

auto constexpr IR::pretty_types(enum types t) -> char const * {
  switch (t) {
  case IF:
    return "IF";
  case IFDEF:
    return "IFDEF";
  case IFNDEF:
    return "IFNDEF";
  case ELIF:
    return "ELIF";
  case ELSE:
    return "ELSE";
  case ENDIF:
    return "ENDIF";
  case GLOBAL_INCLUDE:
    return "GLOBAL_INCLUDE";
  case LOCAL_INCLUDE:
    return "LOCAL_INCLUDE";
  case DEFINE:
    return "DEFINE";
  }
}
} // namespace ir
} // namespace luamake

#endif
