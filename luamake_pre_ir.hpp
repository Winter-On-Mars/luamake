#ifndef __LUAMAKE_PRE_IR_HPP
#define __LUAMAKE_PRE_IR_HPP

#include "luamake_strings.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef DEBUG
#include <ostream>
#endif

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

struct IR_Interpreter;

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
    UNDEF,
  };
  /**
   * @throws Parse_Exc <: Exception
   */
  [[nodiscard]]
  static auto parse(FixedString const &) -> IR;

private:
  auto check_size() -> void;
  // auto push(enum types &&t, std::string_view const) -> void;
  auto push(enum types &&t, std::string_view &&) -> void;

  static auto constexpr pretty_types(enum types) -> char const *;

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
      UNDEF,
      PRAGMA,
      // operators
      // TODO: add other operators
      OP_AND,
      OP_OR,
      OP_BIT_AND,
      OP_BIT_OR,
      OP_DEFINED,
      OP_LESS,
      OP_LESS_EQUAL,
      OP_GREATER,
      OP_GREATER_EQUAL,
      OP_STRINGIZING,
      OP_CONCAT,
      OP_PLUS,
      OP_MINUS,
      OP_STAR,
      OP_SLASH,
      BANG,
      LPAREN,
      RPAREN,
      LANGLE,
      RANGLE,
      QUOTE,
      // values
      LIT_CHAR,
      LIT_STRING,
      LIT_INT,
      LIT_HEX,
      LIT_OCTAL,
      LIT_BINARY,
      LIT_FLOAT,
      LEXEME,
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

#ifdef DEBUG
    auto display(std::ostream &) const noexcept -> std::ostream &;
#endif

  private:
    auto handle_hashif(size_t const, char const *const, size_t) -> size_t;
  };

  size_t size;
  size_t cap;
  std::unique_ptr<types[]> types;
  std::unique_ptr<std::string[]> exprs;
  friend Lexer;
  friend IR_Interpreter;
};

struct IR_Interpreter final {
  constexpr IR_Interpreter(std::unordered_map<std::string, Macro> &macros,
                           std::unordered_set<std::string> &def_macros)
      : macros(macros), def_macros(def_macros) {}

  /**
   * @throws Interpret_Exc <: Exception
   */
  [[nodiscard]]
  auto interpret(IR const &) -> std::vector<std::filesystem::path>;

private:
  std::unordered_map<std::string, Macro> &macros;
  std::unordered_set<std::string> &def_macros;

  auto interpret_impl(bool, IR const &, size_t &,
                      std::vector<std::filesystem::path> &) -> void;

  auto eval(IR const &, size_t &i) const -> int;
  // looks for the next #else, #elif, or #endif
  auto search_for_next_scope(IR const &, size_t) const -> size_t;
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
  case UNDEF:
    return "UNDEF";
  }
}
} // namespace ir
} // namespace luamake

#endif
