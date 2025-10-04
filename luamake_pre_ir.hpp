#ifndef __LUAMAKE_PRE_IR_HPP
#define __LUAMAKE_PRE_IR_HPP

#include "luamake_strings.hpp"

#include <cstddef>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#ifdef DEBUG
#include <iostream>
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
struct Expr_Node;

struct PP_Lexer final {
  enum types : unsigned char {
    // preprocessor stuff
    IF,
    IFDEF,
    IFNDEF,
    ELIF,
    ELSE,
    ENDIF,
    DEFINE,
    DEFINE_FUNC,
    INCLUDE,
    UNDEF,
    PRAGMA,
    // operators
    // TODO: add other operators
    AND,
    OR,
    BIT_AND,
    BIT_OR,
    DEFINED,
    LESS,
    LESS_EQ,
    GREATER,
    GREATER_EQ,
    STRINGIZING,
    CONCAT,
    PLUS,
    MINUS,
    STAR,
    SLASH,
    BANG_EQ,
    EQ,
    EQ_EQ,
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
   * TODO: rework this to entierly use a recursive descent parser
   */
  auto parse_to_ast() -> ir::IR_AST;

  static auto constexpr to_string(enum PP_Lexer::types t) -> std::string;

#ifdef DEBUG
  auto display(std::ostream &) const noexcept -> std::ostream &;
#endif

private:
  PP_Lexer() = default;

  auto matching(size_t, std::initializer_list<enum PP_Lexer::types> &&) -> bool;
  auto handle_hashif(std::string_view const, size_t) -> size_t;
  auto handle_hashdefine(std::string_view const, size_t) -> size_t;
  // auto parse_expr(size_t &, size_t &, IR_AST &) -> Expr_Node;
  auto grab_string(size_t &, size_t &, IR_AST &) -> Expr_Node;

#if 0
  auto equality(size_t &, size_t &, IR_AST &) -> Expr_Node;
  auto comparison(size_t &, size_t &, IR_AST &) -> Expr_Node;
  auto term(size_t &, size_t &, IR_AST &) -> Expr_Node;
  auto factor(size_t &, size_t &, IR_AST &) -> Expr_Node;
  auto unary(size_t &, size_t &, IR_AST &) -> Expr_Node;
  auto primary(size_t &, size_t &, IR_AST &) -> Expr_Node;
#endif

  /**
   * @throws
   */
  auto expect(size_t, enum types) -> void;
};

struct IR_Interpreter;

// TODO: optimize this struct, you can probably combine the Expr_t variable with
// the binary/unary operator in some bit field being or'd, but for now i'm just
// trying to get this working
struct Expr_Node {
  struct Integer final {
    size_t x;
  };
  struct Number final {
    double x;
  };
  struct Defined final {
    StringViews x;
  };
  struct CharLit final {
    StringViews x;
  };
  struct Binary final {
    enum Binary_t { PLUS, MINUS, GREATER, GREATER_EQ, LESS, LESS_EQ };
    std::unique_ptr<Expr_Node> lhs;
    std::unique_ptr<Expr_Node> rhs;
    Binary_t t;
  };
  struct Unary final {
    enum Unary_t { BANG, MINUS };
    std::unique_ptr<Expr_Node> un;
    Unary_t t;
  };
  enum Expr_t {
    INT,
    NUMBER,
    DEFINED,
    CHARLIT,
    NONE,
  } t;
  using Value =
      std::variant<Integer, Number, Defined, CharLit, Binary, Unary, void *>;
  Value val;
  Expr_Node() noexcept : t(NONE), val((void *)nullptr) {}
  Expr_Node(Expr_t &&t, Value &&val) noexcept : t(t), val(std::move(val)) {}
  Expr_Node(Expr_Node &&) = default;
  Expr_Node &operator=(Expr_Node &&) = default;

  Expr_Node(Expr_Node const &) = delete;
  Expr_Node &operator=(Expr_Node const &) = delete;

  static auto constexpr readable_type(Expr_t) noexcept -> char const *;
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
  std::unique_ptr<Expr_Node[]> exprs;

private:
  /**
   * @throws std::bad_alloc
   */
  auto check_size() -> void;
  auto push(IR_Types, Expr_Node &&) -> void;

  auto make_charlit(std::string_view const) -> Expr_Node;
  auto make_nonelit() const -> Expr_Node;

#ifdef DEBUG
  auto shitty_display(std::ostream &) const -> std::ostream &;
#endif // DEBUG

  auto constexpr num_includes() const noexcept -> size_t;

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
  case DEFINE_FUNC:
    return std::string("DEFINE_FUNC");
  case UNDEF:
    return std::string("UNDEF");
  case PRAGMA:
    return std::string("PRAGMA");
  case INCLUDE:
    return std::string("INCLUDE");
  case AND:
    return std::string("AND");
  case OR:
    return std::string("OR");
  case BIT_AND:
    return std::string("BIT_AND");
  case BIT_OR:
    return std::string("BIT_OR");
  case DEFINED:
    return std::string("DEFINED");
  case LESS:
    return std::string("LESS");
  case LESS_EQ:
    return std::string("LESS_EQ");
  case GREATER:
    return std::string("GREATER");
  case GREATER_EQ:
    return std::string("GREATER_EQ");
  case STRINGIZING:
    return std::string("STRINGIZING");
  case CONCAT:
    return std::string("CONCAT");
  case PLUS:
    return std::string("PLUS");
  case MINUS:
    return std::string("MINUS");
  case STAR:
    return std::string("STAR");
  case SLASH:
    return std::string("SLASH");
  case BANG_EQ:
    return std::string("BANG_EQ");
  case EQ:
    return std::string("EQ");
  case EQ_EQ:
    return std::string("EQ_EQ");
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

auto constexpr Expr_Node::readable_type(Expr_t t) noexcept -> char const * {
  switch (t) {
  case INT:
    return "INT";
  case NUMBER:
    return "NUMBER";
  case DEFINED:
    return "DEFINED";
  case CHARLIT:
    return "CHARLIT";
  case NONE:
    return "NONE";
  }
}

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

auto constexpr IR_AST::num_includes() const noexcept -> size_t {
  auto num_includes = size_t{};
  for (auto i = size_t{}; i < size; ++i) {
    if (ast[i] == IR_AST::IR_Types::GLOBAL_INCLUDE ||
        ast[i] == IR_AST::IR_Types::LOCAL_INCLUDE) {
      ++num_includes;
    }
  }
  return num_includes;
}
} // namespace ir
} // namespace luamake

#endif
