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
// TODO: change this namespace's name to pp
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

struct IR_AST;

struct PP_Lexer final {
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
  PP_Lexer(PP_Lexer const &) = delete;
  PP_Lexer &operator=(PP_Lexer const &) = delete;
  PP_Lexer(PP_Lexer &&) = default;
  PP_Lexer &operator=(PP_Lexer &&) = default;
  ~PP_Lexer() = default;

  std::vector<PP_Lexer::types> types;
  std::vector<std::string> lexemes;

  /**
   * @throws Lex_Exc
   */
  static auto lex(FixedString const &) -> PP_Lexer;

  /**
   * @throws Lex_Exc
   */
  auto parse_to_ast() -> ir::IR_AST;

  static auto constexpr to_string(enum PP_Lexer::types t) -> std::string;

#ifdef DEBUG
  auto display(std::ostream &) const noexcept -> std::ostream &;
#endif

private:
  PP_Lexer() = default;
  auto handle_hashif(size_t const, char const *const, size_t) -> size_t;
};

struct IR_Interpreter;

// TODO: rework these into a tagged union
struct Expr_Node {
  enum Expr_t {
    INT,
    NUMBER,
    DEFINED,
    CHARLIT,
    UNINIT,
  } t;
  Expr_Node() noexcept : t(UNINIT) {}
  Expr_Node(Expr_t &&t) noexcept : t(t) {}
  Expr_Node(Expr_Node &&) = default;
  Expr_Node &operator=(Expr_Node &&) = default;
  virtual ~Expr_Node() = default;

  Expr_Node(Expr_Node const &) = delete;
  Expr_Node &operator=(Expr_Node const &) = delete;
};

struct IntLit final : public Expr_Node {
  ~IntLit() final = default;
  size_t lit;
};

struct Number final : public Expr_Node {
  ~Number() final = default;
  double lit;
};

struct Defined final : public Expr_Node {
  ~Defined() final = default;
  StringViews lexeme;
};

struct CharLit final : public Expr_Node {
  CharLit(StringViews &&lit) noexcept : Expr_Node(CHARLIT), lit(lit) {}
  ~CharLit() final = default;
  StringViews lit;
};

// TODO: current the parser is an aggressive parser,
// see about how it performs changing to a lazy parser,
// this would require changing the API, basically just having an
// interpret function
struct IR_AST final {
  /**
   * @throws std::bad_alloc
   */
  IR_AST();
  IR_AST(IR_AST const &) = delete;
  IR_AST &operator=(IR_AST const &) = delete;
  IR_AST(IR_AST &&) = default;
  IR_AST &operator=(IR_AST &&) = default;
  ~IR_AST() = default;
  // these are all pp directives
  enum class IR_Types {
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

  static auto constexpr pretty_types(IR_Types) -> char const *;

  OwnedString lexemes;
  size_t size;
  size_t cap;
  std::unique_ptr<IR_Types[]> ast;
  std::unique_ptr<std::unique_ptr<Expr_Node>[]> exprs;

private:
  /**
   * @throws std::bad_alloc
   */
  auto check_size() -> void;
  auto push(IR_Types, std::unique_ptr<Expr_Node> &&) -> void;

  auto make_charlit(std::string_view const) -> std::unique_ptr<Expr_Node>;

  friend PP_Lexer;
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
  auto interpret(IR_AST const &) -> std::vector<std::filesystem::path>;

private:
  std::unordered_map<std::string, Macro> &macros;
  std::unordered_set<std::string> &def_macros;

  auto interpret_impl(bool, IR_AST const &, size_t &,
                      std::vector<std::filesystem::path> &) -> void;

  auto eval(IR_AST const &, size_t &i) const -> int;
  // looks for the next #else, #elif, or #endif
  auto search_for_next_scope(IR_AST const &, size_t) const -> size_t;
};

auto parse(FixedString const &) -> IR_AST;

auto constexpr IR_AST::pretty_types(IR_Types t) -> char const * {
  switch (t) {
  case IR_Types::IF:
    return "IF";
  case IR_Types::IFDEF:
    return "IFDEF";
  case IR_Types::IFNDEF:
    return "IFNDEF";
  case IR_Types::ELIF:
    return "ELIF";
  case IR_Types::ELSE:
    return "ELSE";
  case IR_Types::ENDIF:
    return "ENDIF";
  case IR_Types::GLOBAL_INCLUDE:
    return "GLOBAL_INCLUDE";
  case IR_Types::LOCAL_INCLUDE:
    return "LOCAL_INCLUDE";
  case IR_Types::DEFINE:
    return "DEFINE";
  case IR_Types::UNDEF:
    return "UNDEF";
  }
}

auto constexpr PP_Lexer::to_string(enum types t) -> std::string {
  switch (t) {
  case IF:
    return std::string("IF");
  case IFDEF:
    return std::string("IFDEF");
  case IFNDEF:
    return std::string("IFNDEF");
  case ELIF:
    return std::string("ELIF");
  case ELSE:
    return std::string("ELSE");
  case ENDIF:
    return std::string("ENDIF");
  case DEFINE:
    return std::string("DEFINE");
  case UNDEF:
    return std::string("UNDEF");
  case PRAGMA:
    return std::string("PRAGMA");
  case INCLUDE:
    return std::string("INCLUDE");
  case OP_AND:
    return std::string("OP_AND");
  case OP_OR:
    return std::string("OP_OR");
  case OP_BIT_AND:
    return std::string("OP_BIT_AND");
  case OP_BIT_OR:
    return std::string("OP_BIT_OR");
  case OP_DEFINED:
    return std::string("OP_DEFINED");
  case OP_LESS:
    return std::string("OP_LESS");
  case OP_LESS_EQUAL:
    return std::string("OP_LESS_EQUAL");
  case OP_GREATER:
    return std::string("OP_GREATER");
  case OP_GREATER_EQUAL:
    return std::string("OP_GREATER_EQUAL");
  case OP_STRINGIZING:
    return std::string("OP_STRINGIZING");
  case OP_CONCAT:
    return std::string("OP_CONCAT");
  case OP_PLUS:
    return std::string("OP_PLUS");
  case OP_MINUS:
    return std::string("OP_MINUS");
  case OP_STAR:
    return std::string("OP_STAR");
  case OP_SLASH:
    return std::string("OP_SLASH");
  case BANG:
    return std::string("BANG");
  case LPAREN:
    return std::string("LPAREN");
  case RPAREN:
    return std::string("RPAREN");
  case LANGLE:
    return std::string("LANGLE");
  case RANGLE:
    return std::string("RANGLE");
  case QUOTE:
    return std::string("QUOTE");
  case LIT_CHAR:
    return std::string("LIT_CHAR");
  case LIT_STRING:
    return std::string("LIT_STRING");
  case LIT_INT:
    return std::string("LIT_INT");
  case LIT_HEX:
    return std::string("LIT_HEX");
  case LIT_OCTAL:
    return std::string("LIT_OCTAL");
  case LIT_BINARY:
    return std::string("LIT_BINARY");
  case LIT_FLOAT:
    return std::string("LIT_FLOAT");
  case LEXEME:
    return std::string("LEXEME");
  }
}
} // namespace ir
} // namespace luamake

#endif
