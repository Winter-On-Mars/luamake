#include "luamake_pre_ir.hpp"
#include "common.hpp"
#include "luamake_strings.hpp"

#include <cctype>
#include <cstddef>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

#ifdef DEBUG
#include <iostream>
#endif // DEBUG

namespace fs = std::filesystem;

using std::string, std::string_view, std::vector, std::unordered_map;

// TODO: this system is over complicated, i think that we only really need 2
// components instead of the 3 currently, a change to this would require a
// complete rearchitecure of the code, and i'm too fucking exhaused to do that
// rn, so i'll get to it in a later commit

namespace luamake {
namespace pp {
namespace {
auto constexpr skip_ws(string_view const buf, size_t i) -> size_t {
  auto constexpr ws = std::string_view{" \t\n\r"};
  // idk could probably just use this for this function
  // it might be faster, something to test, also maybe more readable,
  // maybe somebody could commit a pr with this as a commit that would be a good
  // first commit :)
  // [[auto const x = buf.find(ws, i);]]
  while (i < buf.size()) {
    if (ws.find(buf[i]) != ws.npos) {
      return i;
    }
    ++i;
  }
  return i;
}

auto constexpr is_ws(char const ch) -> bool {
  auto constexpr ws = std::string_view{" \t\n\r"};
  return ws.find(ch) != ws.npos;
}

auto constexpr skip_until(string_view const delims, string_view const buf,
                          size_t i) -> size_t {
  while (i < buf.size()) {
    if (delims.find(buf[i]) != delims.npos) {
      return i;
    }
    ++i;
  }
  return i;
}

template <char delim>
auto constexpr skip_until(string_view const buf, size_t i) -> size_t {
  while (i < buf.size()) {
    if (delim == buf[i])
      return i;
    ++i;
  }
  return i;
}

auto constexpr skip_until_close_multicomment(string_view const buf, size_t i)
    -> size_t {
  while (i < buf.size() && i + 1 < buf.size()) {
    if (buf[i] == '*' && buf[i + 1] == '/') {
      return i + 2; // put buffer[i + 2 - 1] == '/'
    } else {
      ++i;
    }
  }
  return buf.size();
}

template <class T>
auto constexpr skip_until(size_t i, std::span<T> const buf, T delim) -> size_t {
  while (i < buf.size()) {
    if (buf[i] == delim) {
      break;
    }
    ++i;
  }
  return i;
}

auto constexpr is_any_of(string_view const delims, char const *const buffer,
                         size_t const i) -> bool {
  return delims.find(buffer[i]) != delims.npos;
}

enum class delims : size_t {
  LEXEME,
  BINARY_FAIL,
  ALLOWED_HEX,
  ALPHA_SANS_HEX,
  ALLOWED_OCTAL,
  DECIMAL_DIGITS,
  HEX_SANS_DIGITS,
};

auto constexpr delims_list = std::array<string_view, 7>{
    {string_view{" \t\n\r(){}[]+-*/<>=#"}, string_view{"23456789abcdefABCDEF"},
     string_view{"0123456789abcdefABCDEF"},
     string_view{"ghijklmnopqrstuvwxyzGHIJKLMNOPQRSTUVWXYZ"},
     string_view{"01234567"}, string_view{"0123456789"},
     string_view{"abcdefABCDEF"}}};
auto constexpr delims_at(delims &&del) -> string_view {
  return delims_list[static_cast<std::underlying_type_t<delims>>(del)];
}
} // namespace

struct Ast;

enum class ir_t : u8 {
  // preprocessor stuff
  IF,
  IFDEF,
  IFNDEF,
  ELIF,
  ELSE,
  ENDIF,
  DEFINE,
  SPACE, // only used when dealing with #define directives
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
  QUOTE,
  // values
  MACRO, // this is used when lexing, we leave the parsing to later
  LIT_CHAR,
  LIT_STRING,
  LIT_INT,
  LIT_HEX,
  LIT_OCTAL,
  LIT_BINARY,
  LIT_FLOAT,
  LEXEME,
};

constexpr auto to_string(ir_t) -> std::string_view;

struct Lexer final {
  Lexer(Lexer const &) = delete;
  Lexer &operator=(Lexer const &) = delete;
  Lexer(Lexer &&) = default;
  Lexer &operator=(Lexer &&) = default;
  ~Lexer() = default;

  std::vector<ir_t> types;
  std::vector<std::string> lexemes;

  /**
   * @throws Lex_Exc
   */
  static auto lex(FixedString const &) -> Lexer;

  /**
   * @throws Lex_Exc
   */
  auto parse_to_ast() -> Ast;

#ifdef DEBUG
  auto display(std::ostream &) const noexcept -> std::ostream &;
#endif

  Lexer() = default;

  auto matching(size_t, std::initializer_list<ir_t> &&) -> bool;
  auto handle_hashif(std::string_view const, size_t) -> size_t;
  auto parse_define_args(std::string_view const, size_t) -> size_t;
  auto produce_string(std::string_view const, size_t &) -> std::string;
  // auto parse_expr(size_t &, size_t &, IR_AST &) -> Expr_Node;
  auto grab_string(size_t &, size_t &, Ast &) -> std::string;

  /**
   * @throws
   */
  auto expect(size_t, ir_t) -> void;

  /**
   * @throws
   */
  template <class... Args> auto push_lexeme(Args &&...args) -> void {
    types.push_back(ir_t::LEXEME);
    lexemes.push_back(std::string(args...));
    // idk it should be like this but i think i'm using std::forward wrong
    // lexemes.push_back(std::string(std::forward<std::string>(args)...));
  }

  /**
   * @throws
   */
  template <class... Args> auto push_macro(Args &&...args) -> void {
    types.push_back(ir_t::MACRO);
    lexemes.push_back(std::string(args...));
  }
};

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

  static auto constexpr readable_type(Expr_t) noexcept -> std::string_view;
  /**
   * @throws Interpret_Exc
   * (if a float is found)
   */
  auto eval() const -> int;
};

struct Ast final {
  /**
   * @throws std::bad_alloc
   */
  Ast();
  Ast(Ast const &) = delete;
  Ast &operator=(Ast const &) = delete;
  Ast(Ast &&) = default;
  Ast &operator=(Ast &&) = default;
  ~Ast() = default;
  // these are all pp directives
  enum class Ast_t {
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

  static auto constexpr pretty_types(Ast_t) -> string_view;

  OwnedString lexemes;
  size_t size;
  size_t cap;
  std::unique_ptr<Ast_t[]> ast;
  std::unique_ptr<StringViews[]> exprs;

  /**
   * @throws std::bad_alloc
   */
  auto check_size() -> void;
  auto push(Ast_t, std::string const &) -> void;

  auto make_charlit(std::string_view const) -> Expr_Node;
  auto make_nonelit() const -> Expr_Node;

#ifdef DEBUG
  auto shitty_display(std::ostream &) const -> std::ostream &;
#endif // DEBUG

  auto constexpr num_includes() const noexcept -> size_t;
};

struct State final {
  enum state : u8 {
    DEFAULT,
    IF,
    ELSE,
    ENDIF,
    DEFINE,
    GINCLUDE,
    LINCLUDE
  } t = DEFAULT;
  auto interpret(size_t, Interpreter &, Ast const &,
                 std::vector<std::filesystem::path> &) -> size_t;
};

auto constexpr to_string(ir_t t) -> std::string_view {
  switch (t) {
  case ir_t::IF:
    return std::string_view("IF");
  case ir_t::IFDEF:
    return std::string_view("IFDEF");
  case ir_t::IFNDEF:
    return std::string_view("IFNDEF");
  case ir_t::ELIF:
    return std::string_view("ELIF");
  case ir_t::ELSE:
    return std::string_view("ELSE");
  case ir_t::ENDIF:
    return std::string_view("ENDIF");
  case ir_t::DEFINE:
    return std::string_view("DEFINE");
  case ir_t::SPACE:
    return std::string_view("SPACE");
  case ir_t::UNDEF:
    return std::string_view("UNDEF");
  case ir_t::PRAGMA:
    return std::string_view("PRAGMA");
  case ir_t::INCLUDE:
    return std::string_view("INCLUDE");
  case ir_t::AND:
    return std::string_view("AND");
  case ir_t::OR:
    return std::string_view("OR");
  case ir_t::BIT_AND:
    return std::string_view("BIT_AND");
  case ir_t::BIT_OR:
    return std::string_view("BIT_OR");
  case ir_t::DEFINED:
    return std::string_view("DEFINED");
  case ir_t::LESS:
    return std::string_view("LESS");
  case ir_t::LESS_EQ:
    return std::string_view("LESS_EQ");
  case ir_t::GREATER:
    return std::string_view("GREATER");
  case ir_t::GREATER_EQ:
    return std::string_view("GREATER_EQ");
  case ir_t::STRINGIZING:
    return std::string_view("STRINGIZING");
  case ir_t::CONCAT:
    return std::string_view("CONCAT");
  case ir_t::PLUS:
    return std::string_view("PLUS");
  case ir_t::MINUS:
    return std::string_view("MINUS");
  case ir_t::STAR:
    return std::string_view("STAR");
  case ir_t::SLASH:
    return std::string_view("SLASH");
  case ir_t::BANG_EQ:
    return std::string_view("BANG_EQ");
  case ir_t::EQ:
    return std::string_view("EQ");
  case ir_t::EQ_EQ:
    return std::string_view("EQ_EQ");
  case ir_t::BANG:
    return std::string_view("BANG");
  case ir_t::LPAREN:
    return std::string_view("LPAREN");
  case ir_t::RPAREN:
    return std::string_view("RPAREN");
  case ir_t::QUOTE:
    return std::string_view("QUOTE");
  case ir_t::MACRO:
    return std::string_view("MACRO");
  case ir_t::LIT_CHAR:
    return std::string_view("LIT_CHAR");
  case ir_t::LIT_STRING:
    return std::string_view("LIT_STRING");
  case ir_t::LIT_INT:
    return std::string_view("LIT_INT");
  case ir_t::LIT_HEX:
    return std::string_view("LIT_HEX");
  case ir_t::LIT_OCTAL:
    return std::string_view("LIT_OCTAL");
  case ir_t::LIT_BINARY:
    return std::string_view("LIT_BINARY");
  case ir_t::LIT_FLOAT:
    return std::string_view("LIT_FLOAT");
  case ir_t::LEXEME:
    return std::string_view("LEXEME");
  }
}

auto constexpr Expr_Node::readable_type(Expr_t t) noexcept -> std::string_view {
  switch (t) {
  case INT:
    return std::string_view{"INT"};
  case NUMBER:
    return std::string_view{"NUMBER"};
  case DEFINED:
    return std::string_view{"DEFINED"};
  case CHARLIT:
    return std::string_view{"CHARLIT"};
  case NONE:
    return std::string_view{"NONE"};
  }
}

auto constexpr Ast::pretty_types(Ast_t t) -> string_view {
  switch (t) {
  case Ast_t::IF:
    return string_view{"IF"};
  case Ast_t::IFDEF:
    return string_view{"IFDEF"};
  case Ast_t::IFNDEF:
    return string_view{"IFNDEF"};
  case Ast_t::ELIF:
    return string_view{"ELIF"};
  case Ast_t::ELSE:
    return string_view{"ELSE"};
  case Ast_t::ENDIF:
    return string_view{"ENDIF"};
  case Ast_t::GLOBAL_INCLUDE:
    return string_view{"GLOBAL_INCLUDE"};
  case Ast_t::LOCAL_INCLUDE:
    return string_view{"LOCAL_INCLUDE"};
  case Ast_t::DEFINE:
    return string_view{"DEFINE"};
  case Ast_t::UNDEF:
    return string_view{"UNDEF"};
  }
}

auto constexpr Ast::num_includes() const noexcept -> size_t {
  auto num_includes = size_t{};
  for (auto i = size_t{}; i < size; ++i) {
    if (ast[i] == Ast::Ast_t::GLOBAL_INCLUDE ||
        ast[i] == Ast::Ast_t::LOCAL_INCLUDE) {
      ++num_includes;
    }
  }
  return num_includes;
}

// i would like to add lexical short cutting, where if we see a macro that's
// already been defined in something like a header guard, then we completely
// skip the file
// TODO: add proper lexing to the preprocessor, i think that for #if expressions
// we just need to lex until we hit a '\n' character, then when we're parsing we
// can form an expression tree, thankfully everything must eventually be
// interpreted as an int (0 == false, x == true), so we can just have our
// interpreter worry about int's and their expressions, how they get converted
// to int's etc
auto Lexer::lex(FixedString const &file) -> Lexer {
  // clang-format off
  static auto const keywords = unordered_map<string_view, ir_t>{{
    {string_view{"#if"}, ir_t::IF},
    {string_view{"#ifdef"}, ir_t::IFDEF},
    {string_view{"#ifndef"}, ir_t::IFNDEF},
    {string_view{"#elif"}, ir_t::ELIF},
    {string_view{"#else"}, ir_t::ELSE},
    {string_view{"#endif"}, ir_t::ENDIF},
    {string_view{"#define"}, ir_t::DEFINE},
    {string_view{"#include"}, ir_t::INCLUDE},
    {string_view{"defined"}, ir_t::DEFINED},
    {string_view{"#undef"}, ir_t::UNDEF},
    {string_view{"#pragma"}, ir_t::PRAGMA}
  }};
  auto constexpr chars_of_interest = string_view{"#/\""};
  // clang-format on
  auto lex = Lexer();
  lex.types.reserve(64);
  lex.lexemes.reserve(10);

  auto const fcontent = file.view();
  auto const start = fcontent.begin();
  for (auto i = size_t{}; i < file.size;) {
    switch (fcontent[i]) {
    case '#': {
      auto end = skip_until(" \t\r\n", fcontent, i);

      auto const hash_keyword = string_view{start + i, start + end};
      i = end;
      auto const keyword = keywords.find(hash_keyword);
      if (keyword == keywords.end()) {
        // continue to next character of interest
        i = skip_until(chars_of_interest, fcontent, i);
        throw Exception(
            __LINE__,
            std::format("Hash keyword [{}], is not implimented", hash_keyword));
      }

      switch (keyword->second) {
      case ir_t::INCLUDE: {
        if (i >= file.size)
          throw Exception(__LINE__,
                          string("Unable to parse include parameter"));

        i = skip_ws(fcontent, i) + 1;

        if (i >= file.size)
          throw Exception(__LINE__,
                          string("Unable to parse include parameter"));

        lex.types.push_back(ir_t::INCLUDE);
        switch (fcontent[i]) {
        case '<': {
          lex.types.push_back(ir_t::LESS);

          end = skip_until<'>'>(fcontent, i + 1);

          if (!(end < file.size))
            throw Exception(__LINE__, string("Non terminated global include"));

          lex.types.push_back(ir_t::LIT_STRING);
          lex.lexemes.push_back(string(start + i + 1, start + end));
          i = end + 1;
          lex.types.push_back(ir_t::GREATER);
        } break;
        case '"': {
          lex.types.push_back(ir_t::QUOTE);

          end = skip_until<'"'>(fcontent, i + 1);

          if (!(end < file.size))
            throw Exception(__LINE__, string("Non terminated local include"));

          lex.types.push_back(ir_t::LIT_STRING);
          lex.lexemes.push_back(string(start + i + 1, start + end));
          i = end + 1;
          lex.types.push_back(ir_t::QUOTE);
        } break;
        default:
          throw Exception(__LINE__,
                          std::format("character found = {}", fcontent[i]));
        }
      } break;
      case ir_t::IFDEF: {
        lex.types.push_back(ir_t::IFDEF);
        i = skip_ws(fcontent, i) + 1;
        end = skip_until(" \t\n\r", fcontent, i + 1);
        lex.push_lexeme(start + i, start + end);
      } break;
      case ir_t::IFNDEF: {
        lex.types.push_back(ir_t::IFNDEF);
        i = skip_ws(fcontent, i) + 1;
        end = skip_until(" \t\n\r", fcontent, i + 1);
        lex.push_lexeme(start + i, start + end);
      } break;
      case ir_t::DEFINE: {
        lex.types.push_back(ir_t::DEFINE);
        i = skip_ws(fcontent, i) + 1;
        end = skip_until(" (\t\n\r", fcontent, i + 1);
        lex.push_lexeme(string(start + i, start + end));
        if (fcontent[end] == '(') {
          lex.types.push_back(ir_t::SPACE);
          lex.types.push_back(ir_t::LPAREN);
          i = lex.parse_define_args(fcontent, i + 1);
          if (fcontent[i] != ')') {
            throw Exception(__LINE__,
                            std::format("Expected closing ')' when parsing a "
                                        "function macros arguments"));
          } else {
            lex.types.push_back(ir_t::RPAREN);
          }
          throw std::runtime_error(
              "lexing args to a define macro is not currently implimented");
        } else {
          i = skip_ws(fcontent, i) + 1;
          if (!is_ws(fcontent[i])) {
            lex.push_macro(lex.produce_string(fcontent, i));
          }
        }
      } break;
      case ir_t::UNDEF: {
        lex.types.push_back(ir_t::UNDEF);
        i = skip_ws(fcontent, i) + 1;
        end = skip_until(" \t\n\r", fcontent, i + 1);
        lex.push_lexeme(start + i, start + end);
      } break;
      case ir_t::IF:
        lex.types.push_back(ir_t::IF);
        i = skip_ws(fcontent, i) + 1;
        i = lex.handle_hashif(fcontent, i);
        break;
      default:
        lex.types.push_back(keyword->second);
        break;
      }
    } break;
    case '/': {
      if (!(i + 1 < file.size)) {
        throw Exception(__LINE__, string("'/' found at end of file"));
      }
      ++i;
      switch (fcontent[i]) {
      case '/': { // skip until \n
        i = skip_until<'\n'>(fcontent, i + 1);
      } break;
      case '*': { // skip until */
        // TODO: check this code, there might be an issue if the file
        // ends with a multi line comment, i.e. */ at the end of the file
        i = skip_until_close_multicomment(fcontent, i + 1);
        if (i == file.size)
          throw Exception(__LINE__,
                          string("Non terminated multi line comment"));
      } break;
      default: // probably just an op /
        i = skip_until("#/\"", fcontent, i + 1);
        break;
      }
    } break;
    case '"': {
      i = skip_until<'"'>(fcontent, i + 1) + 1;
      if (!(i < file.size))
        throw Exception(__LINE__, string("Non terminated string"));
    } break;
    default:
      i = skip_until("#/\"", fcontent, i + 1);
      break;
    }
  }

  return lex;
}

auto Lexer::parse_to_ast() -> Ast {
  auto ir = Ast();
  auto cur_t = size_t{};
  auto cur_lex = size_t{};
  while (cur_t < types.size()) {
    ir.check_size();
    switch (types[cur_t]) {
    case ir_t::INCLUDE:
      ++cur_t;
      switch (types[cur_t]) {
      case ir_t::LESS:
        cur_t += 3; // LANGLE CHAR_LIT RANGLE
        ir.push(Ast::Ast_t::GLOBAL_INCLUDE, lexemes[cur_lex]);
        ++cur_lex;
        break;
      case ir_t::QUOTE:
        cur_t += 3; // QUOTE CHAR_LIT QUOTE
        ir.push(Ast::Ast_t::LOCAL_INCLUDE, lexemes[cur_lex]);
        ++cur_lex;
        break;
      default:
        throw Exception(__LINE__,
                        std::format("Malformed #include statement, "
                                    "expected '<' or '\"', found [{}]",
                                    to_string(types[cur_t])));
      }
      break;
    case ir_t::IF: {
      ++cur_t;
      auto expr = grab_string(cur_t, cur_lex, ir);
      ir.push(Ast::Ast_t::IF, std::move(expr));
    } break;
    case ir_t::IFDEF:
      cur_t += 2;
      ir.push(Ast::Ast_t::IFDEF, lexemes[cur_lex]);
      ++cur_lex;
      break;
    case ir_t::IFNDEF:
      cur_t += 2;
      ir.push(Ast::Ast_t::IFNDEF, lexemes[cur_lex]);
      ++cur_lex;
      break;
    case ir_t::ENDIF:
      ++cur_t;
      ir.push(Ast::Ast_t::ENDIF, "");
      break;
    case ir_t::ELSE:
      ++cur_t;
      ir.push(Ast::Ast_t::ELSE, "");
      break;
    case ir_t::DEFINE:
      cur_t += 2;
      ir.push(Ast::Ast_t::DEFINE, "");
      ++cur_lex;
      break;
    default:
      throw Exception(__LINE__, std::format("Not implimented, type = [{}]",
                                            to_string(types[cur_t])));
      break;
    }
  }
#ifdef DEBUG
  std::cout << "IR\n";
  ir.shitty_display(std::cout).flush();
#endif // DEBUG
  return ir;
}

// TODO: extract all of these delim strings into variables that we can check on
// TODO: update this to have a string that we build up, in case there's a '\'
// char in there that we have to ignore, bc it can probably make the later steps
// more annoying if we leave it in the lexemes string
auto Lexer::handle_hashif(string_view const buf, size_t i) -> size_t {
  auto constexpr defined_str = string_view{"defined"};
  auto looping = true;
  while (looping && i < buf.size()) {
    auto const ch = buf[i];
    switch (ch) {
    case '\\': {
      ++i;
      if (i < buf.size() && buf[i] == '\n') {
        ++i;
      }
    } break;
    case '\n': {
      looping = false;
      ++i;
    } break;
    case '(': {
      types.push_back(ir_t::LPAREN);
      ++i;
    } break;
    case ')': {
      types.push_back(ir_t::RPAREN);
      ++i;
    } break;
    case '|': {
      ++i;
      if (i < buf.size() && buf[i] == '|') {
        ++i;
        types.push_back(ir_t::OR);
      } else {
        types.push_back(ir_t::BIT_OR);
      }
    } break;
    case '&': {
      ++i;
      if (i < buf.size() && buf[i] == '&') {
        ++i;
        types.push_back(ir_t::AND);
      } else {
        types.push_back(ir_t::BIT_AND);
      }
    } break;
    case '=': {
      ++i;
      if (i < buf.size() && buf[i] == '=') {
        ++i;
        types.push_back(ir_t::EQ_EQ);
      } else {
        types.push_back(ir_t::EQ);
      }
    } break;
    case '!': {
      ++i;
      if (i < buf.size() && buf[i] == '=') {
        ++i;
        types.push_back(ir_t::BANG_EQ);
      } else {
        types.push_back(ir_t::BANG);
      }
    } break;
    case '<': {
      ++i;
      if (i < buf.size() && buf[i] == '=') {
        ++i;
        types.push_back(ir_t::LESS_EQ);
      } else {
        types.push_back(ir_t::LESS);
      }
    } break;
    case '>': {
      ++i;
      if (i < buf.size() && buf[i] == '=') {
        ++i;
        types.push_back(ir_t::GREATER_EQ);
      } else {
        types.push_back(ir_t::GREATER);
      }
    } break;
    case '#': {
      ++i;
      if (i < buf.size() && buf[i] == '#') {
        ++i;
        types.push_back(ir_t::STRINGIZING);
      } else {
        types.push_back(ir_t::CONCAT);
      }
    } break;
    case 'd': {
      if (i + defined_str.size() < buf.size() &&
          strncmp(buf.data() + i, defined_str.data(), defined_str.size()) ==
              0) {
        i += defined_str.size();
        types.push_back(ir_t::DEFINED);
      } else {
        auto const start = i;
        i = skip_until(delims_at(delims::LEXEME), buf, i);
        types.push_back(ir_t::LEXEME);
        lexemes.push_back(string(buf.data() + start, buf.data() + i));
      }
    } break;
#pragma region octal
    case '0': {
      ++i;
      if (!(i < buf.size())) {
        types.push_back(ir_t::LIT_INT);
        lexemes.push_back("0");
      } else {
        switch (buf[i]) {
#pragma region binary
        case 'b': {
          // TODO: check if we're on c++14>=, bc otherwise this is supposed to
          // be an error, the same goes with "'" character
          ++i;
          if (!(i < buf.size())) {
            throw Exception(
                __LINE__,
                std::format("Found string [0b], this is treated as an octal "
                            "number by the compiler, and is therefore "
                            "malformed as only 01234567 are allowed in octal "
                            "numbers"));
          }
          auto const start = i - 1;
          auto allow_quote = false;
          auto inner_looping = true;
          while (inner_looping && i < buf.size()) {
            switch (buf[i]) {
            case '0':
              [[fallthrough]];
            case '1': {
              allow_quote = true;
              ++i;
            } break;
            case '\'': {
              if (allow_quote) {
                ++i;
                allow_quote = false;
              } else {
                throw Exception(
                    __LINE__,
                    std::format(
                        "When parsing a binary number, found two ' characters "
                        "back to back, these are treated as identifiers for a "
                        "char literal, and thus a formatting error."));
              }
            } break;
            default: {
              // these two cases can probably just be combined then b/c they
              // throw the same thing
              if (is_any_of(delims_at(delims::BINARY_FAIL), buf.data(), i)) {
                throw Exception(
                    __LINE__,
                    std::format("Found [{}], while parsing a binary numbers, "
                                "only 0 and 1 are allowed in binary numbers "
                                "(and ' characters for delimiters)",
                                buf[i]));
              } else if (std::isalpha(buf[i])) {
                throw Exception(
                    __LINE__,
                    std::format("Found [{}], while parsing a binary numbers, "
                                "only 0 and 1 are allowed in binary numbers "
                                "(and ' characters for delimiters)",
                                buf[i]));
              }
              inner_looping = false;
            }
            }
          }
          types.push_back(ir_t::LIT_BINARY);
          lexemes.push_back(string(buf.data() + start, buf.data() + i));
        } break;
#pragma endregion binary
#pragma region hex
        case 'x': {
          ++i;
          if (!(i < buf.size())) {
            throw Exception(
                __LINE__,
                std::format("Found string [0x], this is treated as an octal "
                            "number by the compiler, and is therefore "
                            "malformed, as only 01234567 are allowed in octal "
                            "numbers"));
          }
          auto const start = i - 1;
          auto allow_quote = false;
          auto inner_looping = true;
          while (inner_looping && i < buf.size()) {
            if (is_any_of(delims_at(delims::ALLOWED_HEX), buf.data(), i)) {
              allow_quote = true;
              ++i;
            } else if (buf[i] == '\'') {
              if (allow_quote) {
                ++i;
                allow_quote = false;
              } else {
                throw Exception(
                    __LINE__,
                    std::format(
                        "When parsing a hex number, found two ' characters "
                        "back to back, these are treated as identifiers for a "
                        "char literal, and thus a formatting error."));
              }
            } else if (is_any_of(delims_at(delims::ALPHA_SANS_HEX), buf.data(),
                                 i)) {
              throw Exception(__LINE__,
                              std::format("While parsing a hex number, found a "
                                          "character that's not supported."));
            } else {
              inner_looping = false;
            }
          }
          types.push_back(ir_t::LIT_HEX);
          lexemes.push_back(string(buf.data() + start, buf.data() + i));
        } break;
#pragma endregion hex
        case '\'':
          [[fallthrough]];
        case '0':
          [[fallthrough]];
        case '1':
          [[fallthrough]];
        case '2':
          [[fallthrough]];
        case '3':
          [[fallthrough]];
        case '4':
          [[fallthrough]];
        case '5':
          [[fallthrough]];
        case '6':
          [[fallthrough]];
        case '7': {
          auto const start = i;

          auto allow_quote = true;
          while (i < buf.size()) {
            if (is_any_of(delims_at(delims::ALLOWED_OCTAL), buf.data(), i)) {
              allow_quote = true;
              ++i;
            } else if (buf[i] == '\'') {
              if (allow_quote) {
                allow_quote = false;
                ++i;
              } else {
                throw Exception(
                    __LINE__,
                    std::format(
                        "While parsing an octal number encounter a double ' "
                        "character, these are treated as introducing a char "
                        "literal, causing an error."));
              }
            } else {
              if (is_any_of(delims_at(delims::LEXEME), buf.data(), i)) {
                break;
              } else {
                throw Exception(
                    __LINE__,
                    std::format(
                        "While parsing an octal number, encounter [{}], a non "
                        "supposed character in octal numbers",
                        buf[i]));
              }
            }
          }
          types.push_back(ir_t::LIT_OCTAL);
          lexemes.push_back(string(buf.data() + start, buf.data() + i));
        } break;
        case '8':
          [[fallthrough]];
        case '9': {
          throw Exception(
              __LINE__,
              std::format("While parsing an octal number came across an 8 or "
                          "9. Note that when you start a number with 0 it will "
                          "be treated as an octal number by the compiler :)."));
        } break;
        }
      }
    } break;
#pragma endregion octal
#pragma region decimal
    case '1':
      [[fallthrough]];
    case '2':
      [[fallthrough]];
    case '3':
      [[fallthrough]];
    case '4':
      [[fallthrough]];
    case '5':
      [[fallthrough]];
    case '6':
      [[fallthrough]];
    case '7':
      [[fallthrough]];
    case '8':
      [[fallthrough]];
    case '9': {
      auto const start = i;
      auto allow_quote = true;
      while (i < buf.size()) {
        if (is_any_of(delims_at(delims::DECIMAL_DIGITS), buf.data(), i)) {
          allow_quote = true;
          ++i;
        } else if (buf[i] == '\'') {
          if (allow_quote) {
            allow_quote = false;
            ++i;
          } else {
            throw Exception(
                __LINE__,
                std::format("While parsing a decimal number, found a double '. "
                            "These are treated as introducing a char literal, "
                            "causing an error."));
          }
          // These two cases should probably be rolled into one
        } else if (is_any_of(delims_at(delims::HEX_SANS_DIGITS), buf.data(),
                             i)) {
          throw Exception(
              __LINE__,
              std::format("While parsing a decimal number, encountered the "
                          "following unsupported character [{}]",
                          buf[i]));
        } else {
          if (is_any_of(delims_at(delims::LEXEME), buf.data(), i)) {
            break;
          } else {
            throw Exception(
                __LINE__,
                std::format("While parsing a decimal number, encountered the "
                            "following unsupported character [{}]",
                            buf[i]));
          }
        }
      }
      types.push_back(ir_t::LIT_INT);
      lexemes.push_back(string(buf.data() + start, buf.data() + i));
    } break;
#pragma endregion decimal
    default: {
      auto const start = i;
      i = skip_until(delims_at(delims::LEXEME), buf, i);
      types.push_back(ir_t::LEXEME);
      lexemes.push_back(string(buf.data() + start, buf.data() + i));
    } break;
    }
  }
  return i;
}

auto Lexer::parse_define_args(string_view const fcontent, size_t i) -> size_t {
  auto end = i + 1;
  auto looping = true;
  while (looping) {
    switch (fcontent[end]) {
    case ')': {
      if (end != i + 1) {
        push_lexeme(fcontent.data() + i,
                    fcontent.data() + end - 1); // fcontent[end] == ')'
      }
      looping = false;
    } break;
    case '.': {
      throw std::runtime_error("Variatic macros are not currently supported");
    } break;
    case ',': {
      push_lexeme(fcontent.data() + i,
                  fcontent.data() + end - 1); // fcontent[end] == ','
      i = skip_ws(fcontent, end + 1) + 1;
      end = i;
    } break;
    default:
      ++i;
      break;
    }
  }
  return i;
}

auto Lexer::grab_string(size_t &cur_t, size_t &cur_lex, Ast &ir) -> string {
  auto constexpr is_terminator = [](ir_t t) -> bool {
    auto constexpr terminator_list = std::array<ir_t, 11>{
        {ir_t::IF, ir_t::IFDEF, ir_t::IFNDEF, ir_t::ELIF, ir_t::ELSE,
         ir_t::ENDIF, ir_t::DEFINE,
         ir_t::SPACE, // only used when dealing with #define directives
         ir_t::INCLUDE, ir_t::UNDEF, ir_t::PRAGMA}};
    return std::any_of(terminator_list.begin(), terminator_list.end(),
                       [t](auto &&x) { return x == t; });
  };
  auto str = string();
  while (!is_terminator(types[cur_t])) {
    switch (types[cur_t]) {
    case ir_t::AND:
      str += "&&";
      ++cur_t;
      break;
    case ir_t::OR:
      str += "||";
      ++cur_t;
      break;
    case ir_t::BIT_AND:
      str += "&";
      ++cur_t;
      break;
    case ir_t::BIT_OR:
      str += "|";
      ++cur_t;
      break;
    case ir_t::DEFINED:
      str += "defined";
      ++cur_t;
      break;
    case ir_t::LESS:
      str += "<";
      ++cur_t;
      break;
    case ir_t::LESS_EQ:
      str += "<=";
      ++cur_t;
      break;
    case ir_t::GREATER:
      str += ">";
      ++cur_t;
      break;
    case ir_t::GREATER_EQ:
      str += ">=";
      ++cur_t;
      break;
    case ir_t::STRINGIZING:
      str += "#";
      ++cur_t;
      break;
    case ir_t::CONCAT:
      str += "##";
      ++cur_t;
      break;
    case ir_t::PLUS:
      str += "+";
      ++cur_t;
      break;
    case ir_t::MINUS:
      str += "-";
      ++cur_t;
      break;
    case ir_t::STAR:
      str += "*";
      ++cur_t;
      break;
    case ir_t::SLASH:
      str += "/";
      ++cur_t;
      break;
    case ir_t::BANG_EQ:
      str += "!=";
      ++cur_t;
      break;
    case ir_t::EQ:
      str += "=";
      ++cur_t;
      break;
    case ir_t::EQ_EQ:
      str += "==";
      ++cur_t;
      break;
    case ir_t::BANG:
      str += "!";
      ++cur_t;
      break;
    case ir_t::LPAREN:
      str += "(";
      ++cur_t;
      break;
    case ir_t::RPAREN:
      str += ")";
      ++cur_t;
      break;
    case ir_t::QUOTE:
      str += "\"";
      ++cur_t;
      break;
    case ir_t::MACRO:
      [[fallthrough]];
    case ir_t::LIT_CHAR:
      [[fallthrough]];
    case ir_t::LIT_STRING:
      [[fallthrough]];
    case ir_t::LIT_INT:
      [[fallthrough]];
    case ir_t::LIT_HEX:
      [[fallthrough]];
    case ir_t::LIT_OCTAL:
      [[fallthrough]];
    case ir_t::LIT_BINARY:
      [[fallthrough]];
    case ir_t::LIT_FLOAT:
      [[fallthrough]];
    case ir_t::LEXEME:
      str += lexemes[cur_lex++];
      ++cur_t;
      break;
    default:
      break;
    }
  }
  return str;
}

auto Lexer::produce_string(string_view const fcontent, size_t &i) -> string {
  throw std::runtime_error("produce_string Not impl");
}

auto Lexer::expect(size_t cur_t, ir_t tkn) -> void {
  if (types[cur_t] != tkn) {
    throw Exception(__LINE__,
                    std::format("Unexpected token, expected {}, found {}",
                                to_string(tkn), to_string(types[cur_t])));
  }
}

#ifdef DEBUG
auto Lexer::display(std::ostream &out) const noexcept -> std::ostream & {
  out << "Types:\n\t";
  for (auto const &type : types) {
    out << '[' << to_string(type) << ']';
  }
  out << '\n';
  out << "Lexemes:\n\t";
  for (auto const &lexeme : lexemes) {
    out << '[' << lexeme << ']';
  }
  out << '\n';
  return out;
}
#endif

auto Expr_Node::eval() const -> int {
  throw std::runtime_error("Expr_Node::eval not impl");
}

Ast::Ast()
    : size(0), cap(8), ast(std::make_unique<Ast::Ast_t[]>(cap)),
      exprs(std::make_unique<StringViews[]>(cap)) {}

auto parse(FixedString const &file) -> Ast {
#ifdef DEBUG
  auto lexer = Lexer::lex(file);
  std::cout << "lexer\n";
  lexer.display(std::cout);
  std::cout.flush();
  return lexer.parse_to_ast();
#else
  return Lexer::lex(file).parse_to_ast();
#endif
}

auto Ast::check_size() -> void {
  if (size == cap) {
    auto const new_cap = cap * 2;
    auto new_ast = std::make_unique<Ast::Ast_t[]>(new_cap);
    auto new_exprs = std::make_unique<StringViews[]>(new_cap);

    std::memmove(new_ast.get(), ast.get(), sizeof(ir_t) * cap);
    std::memmove(new_exprs.get(), exprs.get(), sizeof(StringViews) * cap);
    ast = std::move(new_ast);
    exprs = std::move(new_exprs);
    cap = new_cap;
  }
}

auto Ast::push(Ast::Ast_t t, string const &expr) -> void {
  if (expr == "") {
    ast[size] = t;
    exprs[size] = {0, 0};
    ++size;
  } else {
    auto const start = lexemes.size;
    lexemes.append(expr);
    auto const end = lexemes.size;
    if (start >= std::numeric_limits<unsigned int>::max() ||
        end >= std::numeric_limits<unsigned int>::max()) {
      throw std::runtime_error(std::format(
          "Damn you have a lot of macros defined, idk what to do here because "
          "you have more than [{}] macro strings. Feel free to open an issue "
          "and fix this, all you have to do is change how StringViews is "
          "implimented and everything to do with it :)",
          std::numeric_limits<unsigned int>::max()));
    }
    ast[size] = t;
    exprs[size] = {static_cast<unsigned int>(start),
                   static_cast<unsigned int>(end)};
    ++size;
  }
}

auto Ast::make_charlit(std::string_view const sv) -> Expr_Node {
  auto const start = lexemes.size;
  lexemes.append(sv);
  auto const end = lexemes.size;
  if (start >= std::numeric_limits<unsigned int>::max() ||
      end >= std::numeric_limits<unsigned int>::max()) {
    throw std::runtime_error(std::format(
        "Damn you have a lot of macros defined, idk what to do here because "
        "you have more than [{}] macro strings. Feel free to open an issue "
        "and fix this, all you have to do is change how StringViews is "
        "implimented and everything to do with it :)",
        std::numeric_limits<unsigned int>::max()));
  }
  return Expr_Node(Expr_Node::CHARLIT, Expr_Node::CharLit(StringViews{
                                           static_cast<unsigned int>(start),
                                           static_cast<unsigned int>(end)}));
}

auto Ast::make_nonelit() const -> Expr_Node { return Expr_Node(); }

#ifdef DEBUG
auto Ast::shitty_display(std::ostream &out) const -> std::ostream & {
  out << "lexemes = [" << string_view{lexemes.buffer, lexemes.size} << "]\n";
  out << "ast:\n\t";
  for (auto i = size_t{}; i < size; ++i) {
    out << '[' << Ast::pretty_types(ast[i]) << ']';
  }
  out << '\n';

  out << "exprs:\n\t";
  for (auto i = size_t{}; i < size; ++i) {
    auto const lit = exprs[i];
    out << '['
        << string_view{lexemes.buffer + lit.start, lexemes.buffer + lit.end}
        << ']';
  }
  out << "\n\n";

  return out;
}
#endif // DEBUG

auto State::interpret(size_t i, Interpreter &interpreter, Ast const &ir,
                      vector<fs::path> &vec) -> size_t {
  throw std::runtime_error("State::interpret not impl");

#if 0
  auto constexpr terminating_nodes = std::array<Ast::Ast_t, 3>{
      {Ast::Ast_t::ENDIF, Ast::Ast_t::ELSE, Ast::Ast_t::ELIF}};
#ifdef DEBUG
  std::cerr << "\tLooking at [" << Ast::pretty_types(ir.ast[i]) << "]\n";
#endif // DEBUG
  switch (ir.ast[i]) {
  case Ast::Ast_t::GLOBAL_INCLUDE: {
    // TODO: check that this file *actually exists*
    ++i;
  } break;
  case Ast::Ast_t::LOCAL_INCLUDE: {
    auto const lit = ir.exprs[i];

    auto const include_name = string(string_view{ir.lexemes.buffer + lit.start,
                                                 ir.lexemes.buffer + lit.end});
    vec.push_back(include_name);
    ++i;
  } break;
  case Ast::Ast_t::IF: {
    auto const expr = eval(ir, i);
    if (expr == 1) {
      ++i;
      while (i < ir.size) {
        if (std::any_of(terminating_nodes.cbegin(), terminating_nodes.cend(),
                        [node = ir.ast[i]](auto &&cur_node) {
                          return node == cur_node;
                        })) {
          break;
        } else {
          interpret_impl(false, ir, i, vec);
        }
      }
    } else {
      ++i;
    }
  } break;
  case Ast::Ast_t::IFDEF: {
    auto const lit = ir.exprs[i];
    auto const checking_macro = string(string_view{
        ir.lexemes.buffer + lit.start, ir.lexemes.buffer + lit.end});
    auto const defined =
        macros.contains(checking_macro) || def_macros.contains(checking_macro);
    ++i;
    if (defined) {
      while (i < ir.size) {
        if (std::any_of(terminating_nodes.cbegin(), terminating_nodes.cend(),
                        [node = ir.ast[i]](auto &&cur_node) {
                          return node == cur_node;
                        }))
          break;
        interpret_impl(false, ir, i, vec);
      }
      if (i == ir.size)
        throw Exception(__LINE__, "Unterminated #ifdef expression");
      i = skip_until(i, std::span(ir.ast.get(), ir.size),
                     IR_AST::IR_Types::ENDIF);
      if (i == ir.size)
        throw Exception(__LINE__, "Unterminated #ifdef expression");
      ++i; // move over the ENDIF
    } else {
      while (i < ir.size) {
        if (!(std::any_of(terminating_nodes.begin(), terminating_nodes.end(),
                          [node = ir.ast[i]](auto &&cur_node) {
                            return cur_node == node;
                          }))) {
          ++i;
        } else {
          break;
        }
      }
      if (i == ir.size)
        throw Exception(__LINE__, "Unterminated #ifdef expression");

      interpret_impl(true, ir, i, vec);

      i = skip_until(i, std::span(ir.ast.get(), ir.size), Ast::Ast_t::ENDIF);
      if (i == ir.size) {
        throw Exception(__LINE__, "Unterminated #ifdef expression");
      }
      ++i; // move over the #endif
    }
  } break;
  case Ast::Ast_t::IFNDEF: {
    auto const lit = ir.exprs[i];
    auto const checking_macro = string(string_view{
        ir.lexemes.buffer + lit.start, ir.lexemes.buffer + lit.end});
    auto const defined =
        macros.contains(checking_macro) || def_macros.contains(checking_macro);
    ++i;
    if (!defined) {
      while (i < ir.size) {
        if (std::any_of(terminating_nodes.begin(), terminating_nodes.end(),
                        [node = ir.ast[i]](auto &&cur_node) {
                          return node == cur_node;
                        }))
          break;
        interpret_impl(false, ir, i, vec);
      }
      if (i == ir.size)
        throw Exception(__LINE__, "Unterminated #ifndef expression");
      i = skip_until(i, std::span(ir.ast.get(), ir.size), Ast::Ast_t::ENDIF);
      if (i == ir.size)
        throw Exception(__LINE__, "Unterminated #ifndef expression");
      ++i; // move over the ENDIF
    } else {
      while (i < ir.size) {
        if (!std::any_of(terminating_nodes.begin(), terminating_nodes.end(),
                         [node = ir.ast[i]](auto &&cur_node) {
                           return cur_node == node;
                         })) {
          ++i;
        } else {
          break;
        }
      }
      if (i == ir.size)
        throw Exception(__LINE__, "Unterminated #ifndef expression");

      if (ir.ast[i] == Ast::Ast_t::ENDIF) {
        ++i;
        return;
      }
      interpret_impl(true, ir, i, vec);

      i = skip_until(i, std::span(ir.ast.get(), ir.size), Ast::Ast_t::ENDIF);
      if (i == ir.size) {
        throw Exception(__LINE__, "Unterminated #ifndef expression");
      }
    }
  } break;
  case Ast::Ast_t::ELSE: {
    if (!interpret_elses) {
      throw Exception(__LINE__, "Unsupported interpretation of #else, "
                                "possibly misformatted preprocessor");
    } else {
      ++i;
      while (i < ir.size) {
        if (ir.ast[i] == Ast::Ast_t::ENDIF) {
          break;
        } else {
          interpret_impl(false, ir, i, vec);
        }
      }
    }
  } break;
  case Ast::Ast_t::DEFINE: {
    // TODO: parse object like macros
    auto const lit = ir.exprs[i];
    auto const defining_macro = string(string_view{
        ir.lexemes.buffer + lit.start, ir.lexemes.buffer + lit.end});
    ++i;
    def_macros.emplace(defining_macro);
  } break;
  default:
    throw Exception(__LINE__, std::format("Not implimented, type = [{}]",
                                          IR_AST::pretty_types(ir.ast[i])));
  }
#endif
}

auto Exception::what() const noexcept -> string {
  return std::format("On line {}, had following error [{}]", line, message);
}

auto Interpreter::interpret(FixedString const &file) -> vector<fs::path> {
  auto ir = Lexer::lex(file).parse_to_ast();
  auto vec = vector<fs::path>();
  auto state = State{};
  vec.reserve(ir.num_includes());
  for (auto i = size_t{}; i < ir.size;) {
    i = state.interpret(i, *this, ir, vec);
  }
  return vec;
}
} // namespace pp
} // namespace luamake
