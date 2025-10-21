#include "luamake_pre_ir.hpp"
#include "common.hpp"
#include "luamake_strings.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <format>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
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

template <class T> using ptr = std::unique_ptr<T>;

using buffer_views = luamake::StringViews;

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
struct AstNode;
struct ElifNode;
struct ElseNode;

struct ExprNode;

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
  Lexer(Lexer &&) noexcept = default;
  Lexer &operator=(Lexer &&) noexcept = default;
  ~Lexer() noexcept = default;

  std::vector<ir_t> types;
  std::vector<std::string> lexemes;

  /**
   * @throws Lex_Exc
   */
  static auto lex(FixedString const &) -> Lexer;

  /**
   * @throws Lex_Exc
   */
  auto ast() -> Ast;

#ifdef DEBUG
  auto display(std::ostream &) const noexcept -> std::ostream &;
#endif

  Lexer() = default;

  constexpr auto matching(size_t, std::initializer_list<ir_t> &&) noexcept
      -> bool;
  auto parse_define_args(string_view const, size_t) -> size_t;
  auto expr(string_view const, size_t) -> size_t;

  auto declaration(size_t &, size_t &) -> std::unique_ptr<AstNode>;
  auto handle_if(size_t &, size_t &) -> std::unique_ptr<AstNode>;
  auto handle_ifdef(size_t &, size_t &) -> std::unique_ptr<AstNode>;
  auto handle_ifndef(size_t &, size_t &) -> std::unique_ptr<AstNode>;
  auto handle_define(size_t &, size_t &) -> std::unique_ptr<AstNode>;
  auto handle_undef(size_t &, size_t &) -> std::unique_ptr<AstNode>;
  auto handle_include(size_t &, size_t &) -> std::unique_ptr<AstNode>;
  auto handle_pragma(size_t &, size_t &) -> std::unique_ptr<AstNode>;

  auto handle_elif(size_t &, size_t &) -> ptr<ElifNode>;
  auto handle_else(size_t &, size_t &) -> ptr<ElseNode>;

  auto parse_expr(size_t &, size_t &) -> ExprNode;

  auto equality(size_t &, size_t &) -> ExprNode;
  auto comparison(size_t &, size_t &) -> ExprNode;
  auto term(size_t &, size_t &) -> ExprNode;
  auto factor(size_t &, size_t &) -> ExprNode;
  auto unary(size_t &, size_t &) -> ExprNode;
  auto primary(size_t &, size_t &) -> ExprNode;

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
struct ExprNode {
  struct Integer final {
    size_t i;
  };
  struct Number final {
    double f;
  };
  struct Defined final {
    string str;
  };
  struct CharLit final {
    string str;
  };
  struct Binary final {
    enum Binary_t {
      PLUS,
      MINUS,
      TIMES,
      DIVIDE,
      GREATER,
      GREATER_EQ,
      LESS,
      LESS_EQ,
      NEQ,
      EQ,
    };
    std::unique_ptr<ExprNode> lhs;
    std::unique_ptr<ExprNode> rhs;
    Binary_t t;
  };
  struct Unary final {
    enum Unary_t { BANG, MINUS };
    std::unique_ptr<ExprNode> un;
    Unary_t t;
  };
  enum Expr_t {
    INT,
    NUMBER,
    DEFINED,
    CHARLIT,
    BINARY,
    UNARY,
    NONE,
  } t;
  using Value =
      std::variant<Integer, Number, Defined, CharLit, Binary, Unary, void *>;
  Value val;
  ExprNode() noexcept : t(NONE), val((void *)nullptr) {}
  ExprNode(Expr_t &&type, Value &&val) noexcept
      : t(type), val(std::move(val)) {}
  ExprNode(ExprNode &&) = default;
  ExprNode &operator=(ExprNode &&) = default;

  static auto make_binary(ir_t, ExprNode &&, ExprNode &&) noexcept -> ExprNode;
  static auto make_unary(ir_t, ExprNode &&) noexcept -> ExprNode;
  static auto make_defined(string &&) noexcept -> ExprNode;

  ExprNode(ExprNode const &) = delete;
  ExprNode &operator=(ExprNode const &) = delete;

  static auto constexpr readable_type(Expr_t) noexcept -> std::string_view;
  /**
   * @throws Interpret_Exc
   * (if a float is found)
   */
  auto eval() const -> int;

  friend auto operator<<(std::ostream &, ExprNode const &) noexcept
      -> std::ostream &;
};

// TODO: devirtualize this if this becomes a perf issue
struct AstVisitor;

struct AstNode {
  virtual ~AstNode() = default;
  virtual auto accept(AstVisitor &) -> void = 0;
};

struct ElifNode;
struct ElseNode;

struct IfNode final : AstNode {
  IfNode(ExprNode &&condition, vector<ptr<AstNode>> &&then_branch,
         vector<ptr<ElifNode>> &&elif_branches,
         ptr<ElseNode> &&else_branch) noexcept
      : condition(std::move(condition)), then_branch(std::move(then_branch)),
        elif_branches(std::move(elif_branches)),
        else_branch(std::move(else_branch)) {}
  ~IfNode() final = default;
  auto accept(AstVisitor &) -> void final;

  ExprNode condition;
  vector<ptr<AstNode>> then_branch;
  vector<ptr<ElifNode>> elif_branches;
  ptr<ElseNode> else_branch;
};

struct IfDefNode final : AstNode {
  IfDefNode(string &&str, vector<ptr<AstNode>> &&then_branch,
            vector<ptr<ElifNode>> &&elif_branches,
            ptr<ElseNode> &&else_branch) noexcept
      : macro(std::move(str)), then_branch(std::move(then_branch)),
        elif_branches(std::move(elif_branches)),
        else_branch(std::move(else_branch)) {}
  ~IfDefNode() final = default;
  auto accept(AstVisitor &) -> void final;

  string macro;
  vector<ptr<AstNode>> then_branch;
  vector<ptr<ElifNode>> elif_branches;
  ptr<ElseNode> else_branch;
};

struct IfNDefNode final : AstNode {
  IfNDefNode(string &&str, vector<ptr<AstNode>> &&then_branch,
             vector<ptr<ElifNode>> &&elif_branches,
             ptr<ElseNode> &&else_branch) noexcept
      : macro(std::move(str)), then_branch(std::move(then_branch)),
        elif_branches(std::move(elif_branches)),
        else_branch(std::move(else_branch)) {}
  ~IfNDefNode() final = default;
  auto accept(AstVisitor &) -> void final;

  string macro;
  vector<ptr<AstNode>> then_branch;
  vector<ptr<ElifNode>> elif_branches;
  ptr<ElseNode> else_branch;
};

struct ElifNode final : AstNode {
  ~ElifNode() final = default;
  auto accept(AstVisitor &) -> void final;

  string condition;
  vector<ptr<AstNode>> if_stmts;
};

struct ElseNode final : AstNode {
  ElseNode(vector<ptr<AstNode>> &&stmts) noexcept : stmts(std::move(stmts)) {}
  ~ElseNode() final = default;
  auto accept(AstVisitor &) -> void final;

  vector<ptr<AstNode>> stmts;
};

struct GlobalIncludeNode final : AstNode {
  GlobalIncludeNode(string const &str) noexcept : path(str) {}
  ~GlobalIncludeNode() final = default;
  auto accept(AstVisitor &) -> void final;

  fs::path path;
};

struct LocalIncludeNode final : AstNode {
  LocalIncludeNode(string const &str) noexcept : path(str) {}
  ~LocalIncludeNode() final = default;
  auto accept(AstVisitor &) -> void final;

  fs::path path;
};

struct DefineNode final : AstNode {
  DefineNode(string const &str, string &&lexeme) noexcept
      : name(str), lexeme(lexeme) {}
  DefineNode(string const &str) noexcept : name(str), lexeme(std::nullopt) {}

  ~DefineNode() final = default;
  auto accept(AstVisitor &) -> void final;

  string name;
  std::optional<string> lexeme;
};

struct DefineFuncNode final : AstNode {
  ~DefineFuncNode() final = default;
  auto accept(AstVisitor &) -> void final;

  string name;
  vector<string> parameters;
  string body;
};

struct UndefNode final : AstNode {
  UndefNode(string const &str) noexcept : name(str) {}
  ~UndefNode() final = default;
  auto accept(AstVisitor &visitor) -> void final;

  string name;
};

struct Ast final {
  /**
   * @throws std::bad_alloc
   */
  Ast();
  Ast(Ast const &) = delete;
  Ast &operator=(Ast const &) = delete;
  Ast(Ast &&) noexcept = default;
  Ast &operator=(Ast &&) noexcept = default;
  ~Ast() noexcept = default;
  // these are all pp directives
  vector<std::unique_ptr<AstNode>> nodes;

  auto accept(AstVisitor &) const -> void;
};

// no need for a virtual dtor bc you shouldn't be dynamically allocating this
// ABC
struct AstVisitor {
  virtual auto visit_if(IfNode &) -> void = 0;
  virtual auto visit_ifdef(IfDefNode &) -> void = 0;
  virtual auto visit_ifndef(IfNDefNode &) -> void = 0;
  virtual auto visit_elif(ElifNode &) -> void = 0;
  virtual auto visit_else(ElseNode &) -> void = 0;
  virtual auto visit_global_include(GlobalIncludeNode &) -> void = 0;
  virtual auto visit_local_include(LocalIncludeNode &) -> void = 0;
  virtual auto visit_define(DefineNode &) -> void = 0;
  virtual auto visit_define_func(DefineFuncNode &) -> void = 0;
  virtual auto visit_undef(UndefNode &) -> void = 0;
};

auto IfNode::accept(AstVisitor &visitor) -> void {
  return visitor.visit_if(*this);
}
auto IfDefNode::accept(AstVisitor &visitor) -> void {
  return visitor.visit_ifdef(*this);
}
auto IfNDefNode::accept(AstVisitor &visitor) -> void {
  return visitor.visit_ifndef(*this);
}
auto ElifNode::accept(AstVisitor &visitor) -> void {
  return visitor.visit_elif(*this);
}
auto ElseNode::accept(AstVisitor &visitor) -> void {
  return visitor.visit_else(*this);
}
auto GlobalIncludeNode::accept(AstVisitor &visitor) -> void {
  return visitor.visit_global_include(*this);
}
auto LocalIncludeNode::accept(AstVisitor &visitor) -> void {
  return visitor.visit_local_include(*this);
}
auto DefineNode::accept(AstVisitor &visitor) -> void {
  return visitor.visit_define(*this);
}
auto DefineFuncNode::accept(AstVisitor &visitor) -> void {
  return visitor.visit_define_func(*this);
}
auto UndefNode::accept(AstVisitor &visitor) -> void {
  return visitor.visit_undef(*this);
}

struct AstPrinter final : AstVisitor {
  std::ostream &out;
  // i really love that in c++ this is a thing you can do :)
  std::allocator_traits<std::string::allocator_type>::size_type depth;
  AstPrinter(std::ostream &out) noexcept : out(out), depth(0) {}

  auto print(Ast &ast) -> void {
    for (auto &&node : ast.nodes) {
      node->accept(*this);
    }
    out.flush();
  }

  constexpr auto get_indents() -> string { return string(depth, ' '); }

  auto visit_if(IfNode &) -> void final;
  auto visit_ifdef(IfDefNode &) -> void final;
  auto visit_ifndef(IfNDefNode &) -> void final;
  auto visit_elif(ElifNode &) -> void final;
  auto visit_else(ElseNode &) -> void final;
  auto visit_global_include(GlobalIncludeNode &) -> void final;
  auto visit_local_include(LocalIncludeNode &) -> void final;
  auto visit_define(DefineNode &) -> void final;
  auto visit_define_func(DefineFuncNode &) -> void final;
  auto visit_undef(UndefNode &) -> void final;
};

#if 0
struct AstIncluder final : AstVisitor {
  vector<fs::path> &paths;
  AstIncluder(vector<fs::path> &paths) noexcept : paths(paths) {}
};
#endif

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

auto constexpr ExprNode::readable_type(Expr_t t) noexcept -> std::string_view {
  switch (t) {
  case INT:
    return std::string_view{"INT"};
  case NUMBER:
    return std::string_view{"NUMBER"};
  case DEFINED:
    return std::string_view{"DEFINED"};
  case CHARLIT:
    return std::string_view{"CHARLIT"};
  case BINARY:
    return std::string_view{"BINARY"};
  case UNARY:
    return std::string_view{"UNARY"};
  case NONE:
    return std::string_view{"NONE"};
  }
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
            std::format("Hash keyword [{}], is not implimented", hash_keyword));
      }

      switch (keyword->second) {
      case ir_t::INCLUDE: {
        if (i >= file.size)
          throw Exception(string("Unable to parse include parameter"));

        i = skip_ws(fcontent, i) + 1;

        if (i >= file.size)
          throw Exception(string("Unable to parse include parameter"));

        lex.types.push_back(ir_t::INCLUDE);
        switch (fcontent[i]) {
        case '<': {
          lex.types.push_back(ir_t::LESS);

          end = skip_until<'>'>(fcontent, i + 1);

          if (!(end < file.size)) {
            throw Exception(string("Non terminated global include"));
          }

          lex.types.push_back(ir_t::LIT_STRING);
          lex.lexemes.push_back(string(start + i + 1, start + end));
          i = end + 1;
          lex.types.push_back(ir_t::GREATER);
        } break;
        case '"': {
          lex.types.push_back(ir_t::QUOTE);

          end = skip_until<'"'>(fcontent, i + 1);

          if (!(end < file.size)) {
            throw Exception(string("Non terminated local include"));
          }

          lex.types.push_back(ir_t::LIT_STRING);
          lex.lexemes.push_back(string(start + i + 1, start + end));
          i = end + 1;
          lex.types.push_back(ir_t::QUOTE);
        } break;
        default:
          throw Exception(std::format("character found = {}", fcontent[i]));
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
        switch (fcontent[end]) {
        case '(': {
          lex.types.push_back(ir_t::SPACE);
          lex.types.push_back(ir_t::LPAREN);
          i = lex.parse_define_args(fcontent, i);
          if (fcontent[i] != ')') {
            throw Exception(std::format("Expected closing ')' when parsing "
                                        "function macro arguments"));
          }
          lex.types.push_back(ir_t::RPAREN);
          throw Exception(std::format("Parsing function macro bodies is not "
                                      "currently implimented"));
        } break;
        default: {
          i = lex.expr(fcontent, i);
        } break;
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
        i = lex.expr(fcontent, i);
        break;
      default:
        lex.types.push_back(keyword->second);
        break;
      }
    } break;
    case '/': {
      if (!(i + 1 < file.size)) {
        throw Exception(string("'/' found at end of file"));
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
          throw Exception(string("Non terminated multi line comment"));
      } break;
      default: // probably just an op /
        i = skip_until("#/\"", fcontent, i + 1);
        break;
      }
    } break;
    case '"': {
      i = skip_until<'"'>(fcontent, i + 1) + 1;
      if (!(i < file.size)) {
        throw Exception(string("Non terminated string"));
      }
    } break;
    default:
      i = skip_until("#/\"", fcontent, i + 1);
      break;
    }
  }
#ifdef DEBUG
  std::cout << "Lexer:\n";
  lex.display(std::cout).flush();
#endif // DEBUG
  return lex;
}

auto Lexer::ast() -> Ast {
  auto ast = Ast();
  auto cur_t = size_t{};
  auto cur_lex = size_t{};
  while (cur_t < types.size()) {
    ast.nodes.push_back(declaration(cur_t, cur_lex));
  }
  return ast;
}

constexpr auto Lexer::matching(size_t cur_t,
                               std::initializer_list<ir_t> &&tkns) noexcept
    -> bool {
  // idk why std::bind_front works but not just std::bind?
  return std::ranges::any_of(
      tkns, std::bind_front(std::equal_to<ir_t>(), types[cur_t]));
}

static_assert(std::ranges::any_of(std::array<ir_t, 2>({ir_t::ELSE, ir_t::ELIF}),
                                  std::bind_front(std::equal_to<ir_t>(),
                                                  ir_t::ELSE)),
              "");

auto Lexer::expr(string_view const buf, size_t i) -> size_t {
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
            throw Exception("Found string [0b], this is treated as an octal "
                            "number by the compiler, and is therefore "
                            "malformed as only 01234567 are allowed in octal "
                            "numbers");
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
                    "When parsing a binary number, found two ' characters "
                    "back to back, these are treated as identifiers for a "
                    "char literal, and thus a formatting error.");
              }
            } break;
            default: {
              // these two cases can probably just be combined then b/c they
              // throw the same thing
              if (is_any_of(delims_at(delims::BINARY_FAIL), buf.data(), i)) {
                throw Exception(
                    std::format("Found [{}], while parsing a binary numbers, "
                                "only 0 and 1 are allowed in binary numbers "
                                "(and ' characters for delimiters)",
                                buf[i]));
              } else if (std::isalpha(buf[i])) {
                throw Exception(
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
            throw Exception("Found string [0x], this is treated as an octal "
                            "number by the compiler, and is therefore "
                            "malformed, as only 01234567 are allowed in octal "
                            "numbers");
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
                    "When parsing a hex number, found two ' characters "
                    "back to back, these are treated as identifiers for a "
                    "char literal, and thus a formatting error.");
              }
            } else if (is_any_of(delims_at(delims::ALPHA_SANS_HEX), buf.data(),
                                 i)) {
              throw Exception("While parsing a hex number, found a "
                              "character that's not supported.");
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
                    "While parsing an octal number encounter a double ' "
                    "character, these are treated as introducing a char "
                    "literal, causing an error.");
              }
            } else {
              if (is_any_of(delims_at(delims::LEXEME), buf.data(), i)) {
                break;
              } else {
                throw Exception(std::format(
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
          throw Exception("While parsing an octal number came across an 8 or "
                          "9. Note that when you start a number with 0 it will "
                          "be treated as an octal number by the compiler :).");
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
            throw Exception("While parsing a decimal number, found a double '. "
                            "These are treated as introducing a char literal, "
                            "causing an error.");
          }
          // These two cases should probably be rolled into one
        } else if (is_any_of(delims_at(delims::HEX_SANS_DIGITS), buf.data(),
                             i)) {
          throw Exception(
              std::format("While parsing a decimal number, encountered the "
                          "following unsupported character [{}]",
                          buf[i]));
        } else {
          if (is_any_of(delims_at(delims::LEXEME), buf.data(), i)) {
            break;
          } else {
            throw Exception(
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

auto Lexer::declaration(size_t &cur_t, size_t &cur_lex)
    -> std::unique_ptr<AstNode> {
  switch (types[cur_t]) {
  case ir_t::IF:
    return handle_if(cur_t, cur_lex);
  case ir_t::IFDEF:
    return handle_ifdef(cur_t, cur_lex);
  case ir_t::IFNDEF:
    return handle_ifndef(cur_t, cur_lex);
  case ir_t::DEFINE:
    return handle_define(cur_t, cur_lex);
  case ir_t::UNDEF:
    return handle_undef(cur_t, cur_lex);
  case ir_t::INCLUDE:
    return handle_include(cur_t, cur_lex);
  case ir_t::PRAGMA:
    return handle_pragma(cur_t, cur_lex);
  case ir_t::ELIF: // TODO: throw for these with their own special error
                   // messages
    [[fallthrough]];
  case ir_t::ELSE:
    [[fallthrough]];
  case ir_t::ENDIF:
    [[fallthrough]];
  default:
    throw Exception(
        std::format("Unexpected token [{}] found in top level scope.",
                    to_string(types[cur_t])));
  }
}

auto Lexer::handle_if(size_t &cur_t, size_t &cur_lex)
    -> std::unique_ptr<AstNode> {
  ++cur_t;

  auto expr = parse_expr(cur_t, cur_lex);
  auto then_branch = vector<ptr<AstNode>>();
  auto elif_branches = vector<ptr<ElifNode>>();
  auto else_branch = ptr<ElseNode>(nullptr);
  enum class FoundEnd {
    none,
    elif,
    _else,
    endif,
  } cur = FoundEnd::none;
  while (cur == FoundEnd::none && cur_t < types.size()) {
    switch (types[cur_t]) {
    case ir_t::IF:
      then_branch.push_back(handle_if(cur_t, cur_lex));
      break;
    case ir_t::IFDEF:
      then_branch.push_back(handle_ifdef(cur_t, cur_lex));
      break;
    case ir_t::IFNDEF:
      then_branch.push_back(handle_ifndef(cur_t, cur_lex));
      break;
    case ir_t::DEFINE:
      then_branch.push_back(handle_define(cur_t, cur_lex));
      break;
    case ir_t::UNDEF:
      then_branch.push_back(handle_undef(cur_t, cur_lex));
      break;
    case ir_t::INCLUDE:
      then_branch.push_back(handle_include(cur_t, cur_lex));
      break;
    case ir_t::PRAGMA:
      then_branch.push_back(handle_pragma(cur_t, cur_lex));
      break;
    case ir_t::ELIF:
      cur = FoundEnd::elif;
      break;
    case ir_t::ELSE:
      cur = FoundEnd::_else;
      break;
    case ir_t::ENDIF:
      cur = FoundEnd::endif;
      break;
    default:
      throw Exception(
          std::format("Unexpected token [{}] found in top level scope.",
                      to_string(types[cur_t])));
    }
  }

  if (cur == FoundEnd::none) {
    throw Exception(std::format("Unterminated #ifdef directive found"));
  }

  while (cur != FoundEnd::none) {
    switch (cur) {
    case FoundEnd::elif:
      if (else_branch != nullptr) {
        throw Exception(std::format("Found #elif directive following #else "
                                    "directive in #ifdef directive"));
      }
      while (cur_t < types.size() &&
             (types[cur_t] != ir_t::ELSE || types[cur_t] != ir_t::ENDIF)) {
        elif_branches.push_back(handle_elif(cur_t, cur_lex));
      }
      cur = [this](auto const cur_t) {
        switch (types[cur_t]) {
        case ir_t::ELSE:
          return FoundEnd::_else;
        case ir_t::ENDIF:
          return FoundEnd::endif;
        case ir_t::ELIF:
          return FoundEnd::elif;
        default:
          throw Exception(std::format(
              "Unexpected token [{}], found after parsing #else directive",
              to_string(types[cur_t])));
        }
      }(cur_t);
      break;
    case FoundEnd::_else:
      if (else_branch != nullptr) {
        throw Exception("Found multiple #else directives attached to a single "
                        "#ifdef directive");
      }
      else_branch = handle_else(cur_t, cur_lex);
      cur = [this](auto const cur_t) {
        switch (types[cur_t]) {
        case ir_t::ELSE:
          return FoundEnd::_else;
        case ir_t::ENDIF:
          return FoundEnd::endif;
        case ir_t::ELIF:
          return FoundEnd::elif;
        default:
          throw Exception(std::format(
              "Unexpected token [{}], found after parsing #else directive",
              to_string(types[cur_t])));
        }
      }(cur_t);
      break;
    case FoundEnd::endif:
      ++cur_t;
      cur = FoundEnd::none;
      break;
    case FoundEnd::none:
      unreachable();
    }
  }

  return std::make_unique<IfNode>(std::move(expr), std::move(then_branch),
                                  std::move(elif_branches),
                                  std::move(else_branch));
}
auto Lexer::handle_ifdef(size_t &cur_t, size_t &cur_lex)
    -> std::unique_ptr<AstNode> {
  ++cur_t;
  if (types[cur_t] != ir_t::LEXEME) {
    throw Exception(std::format("Expected lexeme following #ifdef"));
  }
  ++cur_t;
  auto lex = lexemes[cur_lex++];
  auto then_branch = vector<ptr<AstNode>>();
  auto elif_branches = vector<ptr<ElifNode>>();
  auto else_branch = ptr<ElseNode>(nullptr);
  enum class FoundEnd {
    none,
    elif,
    _else,
    endif,
  } cur = FoundEnd::none;
  while (cur == FoundEnd::none && cur_t < types.size()) {
    switch (types[cur_t]) {
    case ir_t::IF:
      then_branch.push_back(handle_if(cur_t, cur_lex));
      break;
    case ir_t::IFDEF:
      then_branch.push_back(handle_ifdef(cur_t, cur_lex));
      break;
    case ir_t::IFNDEF:
      then_branch.push_back(handle_ifndef(cur_t, cur_lex));
      break;
    case ir_t::DEFINE:
      then_branch.push_back(handle_define(cur_t, cur_lex));
      break;
    case ir_t::UNDEF:
      then_branch.push_back(handle_undef(cur_t, cur_lex));
      break;
    case ir_t::INCLUDE:
      then_branch.push_back(handle_include(cur_t, cur_lex));
      break;
    case ir_t::PRAGMA:
      then_branch.push_back(handle_pragma(cur_t, cur_lex));
      break;
    case ir_t::ELIF:
      cur = FoundEnd::elif;
      break;
    case ir_t::ELSE:
      cur = FoundEnd::_else;
      break;
    case ir_t::ENDIF:
      cur = FoundEnd::endif;
      break;
    default:
      throw Exception(
          std::format("Unexpected token [{}] found in top level scope.",
                      to_string(types[cur_t])));
    }
  }

  if (cur == FoundEnd::none) {
    throw Exception(std::format("Unterminated #ifdef directive found"));
  }

  while (cur != FoundEnd::none) {
    switch (cur) {
    case FoundEnd::elif:
      if (else_branch != nullptr) {
        throw Exception(std::format("Found #elif directive following #else "
                                    "directive in #ifdef directive"));
      }
      while (cur_t < types.size() &&
             (types[cur_t] != ir_t::ELSE || types[cur_t] != ir_t::ENDIF)) {
        elif_branches.push_back(handle_elif(cur_t, cur_lex));
      }
      cur = [this](auto const cur_t) {
        switch (types[cur_t]) {
        case ir_t::ELSE:
          return FoundEnd::_else;
        case ir_t::ENDIF:
          return FoundEnd::endif;
        case ir_t::ELIF:
          return FoundEnd::elif;
        default:
          throw Exception(std::format(
              "Unexpected token [{}], found after parsing #else directive",
              to_string(types[cur_t])));
        }
      }(cur_t);
      break;
    case FoundEnd::_else:
      if (else_branch != nullptr) {
        throw Exception("Found multiple #else directives attached to a single "
                        "#ifdef directive");
      }
      else_branch = handle_else(cur_t, cur_lex);
      cur = [this](auto const cur_t) {
        switch (types[cur_t]) {
        case ir_t::ELSE:
          return FoundEnd::_else;
        case ir_t::ENDIF:
          return FoundEnd::endif;
        case ir_t::ELIF:
          return FoundEnd::elif;
        default:
          throw Exception(std::format(
              "Unexpected token [{}], found after parsing #else directive",
              to_string(types[cur_t])));
        }
      }(cur_t);
      break;
    case FoundEnd::endif:
      ++cur_t;
      cur = FoundEnd::none;
      break;
    case FoundEnd::none:
      unreachable();
    }
  }

  return std::make_unique<IfDefNode>(std::move(lex), std::move(then_branch),
                                     std::move(elif_branches),
                                     std::move(else_branch));
}

auto Lexer::handle_ifndef(size_t &cur_t, size_t &cur_lex)
    -> std::unique_ptr<AstNode> {
  ++cur_t;
  if (types[cur_t] != ir_t::LEXEME) {
    throw Exception(std::format("Expected lexeme following #ifndef"));
  }
  ++cur_t;
  auto lex = lexemes[cur_lex++];
  auto then_branch = vector<ptr<AstNode>>();
  auto elif_branches = vector<ptr<ElifNode>>();
  auto else_branch = ptr<ElseNode>(nullptr);
  enum class FoundEnd {
    none,
    elif,
    _else,
    endif,
  } cur = FoundEnd::none;
  while (cur == FoundEnd::none && cur_t < types.size()) {
    switch (types[cur_t]) {
    case ir_t::IF:
      then_branch.push_back(handle_if(cur_t, cur_lex));
      break;
    case ir_t::IFDEF:
      then_branch.push_back(handle_ifdef(cur_t, cur_lex));
      break;
    case ir_t::IFNDEF:
      then_branch.push_back(handle_ifndef(cur_t, cur_lex));
      break;
    case ir_t::DEFINE:
      then_branch.push_back(handle_define(cur_t, cur_lex));
      break;
    case ir_t::UNDEF:
      then_branch.push_back(handle_undef(cur_t, cur_lex));
      break;
    case ir_t::INCLUDE:
      then_branch.push_back(handle_include(cur_t, cur_lex));
      break;
    case ir_t::PRAGMA:
      then_branch.push_back(handle_pragma(cur_t, cur_lex));
      break;
    case ir_t::ELIF:
      cur = FoundEnd::elif;
      break;
    case ir_t::ELSE:
      cur = FoundEnd::_else;
      break;
    case ir_t::ENDIF:
      cur = FoundEnd::endif;
      break;
    default:
      throw Exception(
          std::format("Unexpected token [{}] found in top level scope.",
                      to_string(types[cur_t])));
    }
  }

  if (cur == FoundEnd::none) {
    throw Exception(std::format("Unterminated #ifndef directive found"));
  }

  while (cur != FoundEnd::none) {
    switch (cur) {
    case FoundEnd::elif:
      if (else_branch != nullptr) {
        throw Exception(std::format("Found #elif directive following #else "
                                    "directive in #ifndef directive"));
      }
      while (cur_t < types.size() &&
             (types[cur_t] != ir_t::ELSE || types[cur_t] != ir_t::ENDIF)) {
        elif_branches.push_back(handle_elif(cur_t, cur_lex));
      }
      break;
    case FoundEnd::_else:
      if (else_branch != nullptr) {
        throw Exception("Found multiple #else directives attached to a single "
                        "#ifndef directive");
      }
      else_branch = handle_else(cur_t, cur_lex);
      break;
    case FoundEnd::endif:
      ++cur_t;
      cur = FoundEnd::none;
      break;
    case FoundEnd::none:
      unreachable();
      break;
    }
  }

  return std::make_unique<IfNDefNode>(std::move(lex), std::move(then_branch),
                                      std::move(elif_branches),
                                      std::move(else_branch));
  throw std::runtime_error(std::format("{} not impl", __PRETTY_FUNCTION__));
}

auto Lexer::handle_define(size_t &, size_t &) -> std::unique_ptr<AstNode> {
  throw std::runtime_error(std::format("{} not impl", __PRETTY_FUNCTION__));
}

auto Lexer::handle_undef(size_t &cur_t, size_t &cur_lex)
    -> std::unique_ptr<AstNode> {
  ++cur_t;
  if (types[cur_t] != ir_t::MACRO) {
    throw Exception(
        std::format("Expected macro in #undef preprocessor directive"));
  }
  return std::make_unique<UndefNode>(lexemes[cur_lex++]);
}

auto Lexer::handle_include(size_t &cur_t, size_t &cur_lex)
    -> std::unique_ptr<AstNode> {
  ++cur_t;
  switch (types[cur_t]) {
  case ir_t::LESS: {
    cur_t += 3;
    return std::make_unique<GlobalIncludeNode>(lexemes[cur_lex++]);
  } break;
  case ir_t::QUOTE: {
    cur_t += 3;
    return std::make_unique<LocalIncludeNode>(lexemes[cur_lex++]);
  } break;
  default: {
    throw Exception(std::format("Malformed #include statement, "
                                "expected '<' or '\"', found [{}]",
                                to_string(types[cur_t])));
  }
  }
}

auto Lexer::handle_pragma(size_t &, size_t &) -> std::unique_ptr<AstNode> {
  throw Exception(
      std::format("#pragma statement parsing is not currently implimented"));
}

auto Lexer::handle_elif(size_t &cur_t, size_t &cur_lex) -> ptr<ElifNode> {
  throw Exception(
      std::format("#elif statement parsing is not currently implimented"));
}

// it could be a good idea to have this #else consume the #endif(?)
auto Lexer::handle_else(size_t &cur_t, size_t &cur_lex) -> ptr<ElseNode> {
  ++cur_t;
  auto res = vector<ptr<AstNode>>();
  auto looping = true;
  while (looping && cur_t < types.size()) {
    switch (types[cur_t]) {
    case ir_t::IF:
      res.push_back(handle_if(cur_t, cur_lex));
      break;
    case ir_t::IFDEF:
      res.push_back(handle_ifdef(cur_t, cur_lex));
      break;
    case ir_t::IFNDEF:
      res.push_back(handle_ifndef(cur_t, cur_lex));
      break;
    case ir_t::DEFINE:
      res.push_back(handle_define(cur_t, cur_lex));
      break;
    case ir_t::UNDEF:
      res.push_back(handle_undef(cur_t, cur_lex));
      break;
    case ir_t::INCLUDE:
      res.push_back(handle_include(cur_t, cur_lex));
      break;
    case ir_t::PRAGMA:
      res.push_back(handle_pragma(cur_t, cur_lex));
      break;
    case ir_t::ELSE: // this error *should* be handled elsewhere
      [[fallthrough]];
    case ir_t::ELIF:
      [[fallthrough]];
    case ir_t::ENDIF:
      looping = false;
      break;
    default:
      throw Exception(
          std::format("Unexpected token [{}] found in top level scope.",
                      to_string(types[cur_t])));
    }
  }
  return std::make_unique<ElseNode>(std::move(res));
}

auto Lexer::parse_expr(size_t &cur_t, size_t &cur_lex) -> ExprNode {
  return equality(cur_t, cur_lex);
}

auto Lexer::equality(size_t &cur_t, size_t &cur_lex) -> ExprNode {
  auto lhs = comparison(cur_t, cur_lex);
  while (matching(cur_t, {ir_t::BANG_EQ, ir_t::EQ_EQ})) {
    auto const tkn = types[cur_t++];
    auto rhs = comparison(cur_t, cur_lex);

    lhs = ExprNode::make_binary(tkn, std::move(lhs), std::move(rhs));
  }

  return lhs;
}

auto Lexer::comparison(size_t &cur_t, size_t &cur_lex) -> ExprNode {
  auto lhs = term(cur_t, cur_lex);
  while (matching(
      cur_t, {ir_t::LESS, ir_t::LESS_EQ, ir_t::GREATER, ir_t::GREATER_EQ})) {
    auto const tkn = types[cur_t++];
    auto rhs = term(cur_t, cur_lex);

    lhs = ExprNode::make_binary(tkn, std::move(lhs), std::move(rhs));
  }

  return lhs;
}

auto Lexer::term(size_t &cur_t, size_t &cur_lex) -> ExprNode {
  auto lhs = factor(cur_t, cur_lex);
  while (matching(cur_t, {ir_t::PLUS, ir_t::MINUS})) {
    auto const tkn = types[cur_t++];
    auto rhs = factor(cur_t, cur_lex);

    lhs = ExprNode::make_binary(tkn, std::move(lhs), std::move(rhs));
  }

  return lhs;
}

auto Lexer::factor(size_t &cur_t, size_t &cur_lex) -> ExprNode {
  auto lhs = unary(cur_t, cur_lex);
  while (matching(cur_t, {ir_t::STAR, ir_t::SLASH})) {
    auto const tkn = types[cur_t++];
    auto rhs = unary(cur_t, cur_lex);

    lhs = ExprNode::make_binary(tkn, std::move(lhs), std::move(rhs));
  }

  return lhs;
}

auto Lexer::unary(size_t &cur_t, size_t &cur_lex) -> ExprNode {
  if (matching(cur_t, {ir_t::BANG, ir_t::MINUS})) {
    auto const tkn = types[cur_t++];
    auto un = unary(cur_t, cur_lex);
    return ExprNode::make_unary(tkn, std::move(un));
  }
  return primary(cur_t, cur_lex);
}

auto Lexer::primary(size_t &cur_t, size_t &cur_lex) -> ExprNode {
  switch (types[cur_t]) {
  case ir_t::MACRO:
    break;
  case ir_t::LIT_CHAR:
    break;
  case ir_t::LIT_STRING:
    break;
  case ir_t::LIT_INT:
    break;
  case ir_t::LIT_HEX:
    break;
  case ir_t::LIT_OCTAL:
    break;
  case ir_t::LIT_BINARY:
    break;
  case ir_t::LIT_FLOAT:
    break;
  case ir_t::LEXEME:
    break;
  case ir_t::LPAREN: {
    expect(cur_t, ir_t::RPAREN);
  } break;
  case ir_t::DEFINED: {
    ++cur_t;
    auto lex = string();
    if (types[cur_t] == ir_t::LPAREN) {
      ++cur_t;
      expect(cur_t, ir_t::LEXEME);
      ++cur_t;
      lex = lexemes[cur_lex++];
      expect(cur_t, ir_t::RPAREN);
      ++cur_t;
    } else {
      expect(cur_t, ir_t::LEXEME);
      ++cur_t;
      lex = lexemes[cur_lex++];
    }
    return ExprNode::make_defined(std::move(lex));
  } break;

  default:
    throw Exception(
        std::format("Unexpected token [{}] found while parsing an expression",
                    to_string(types[cur_t])));
  }
  throw Exception(std::format("{} not impl", __PRETTY_FUNCTION__));
}

// TODO: update this function to allow for optional string for extra info
auto Lexer::expect(size_t cur_t, ir_t tkn) -> void {
  if (types[cur_t] != tkn) {
    throw Exception(std::format("Unexpected token, expected {}, found {}",
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

auto ExprNode::make_binary(ir_t tkn, ExprNode &&lhs, ExprNode &&rhs) noexcept
    -> ExprNode {
  auto bin_t = [](ir_t tkn) {
    switch (tkn) {
    case ir_t::PLUS:
      return Binary::PLUS;
    case ir_t::MINUS:
      return Binary::MINUS;
    case ir_t::SLASH:
      return Binary::DIVIDE;
    case ir_t::STAR:
      return Binary::TIMES;
    case ir_t::GREATER:
      return Binary::GREATER;
    case ir_t::GREATER_EQ:
      return Binary::GREATER_EQ;
    case ir_t::LESS:
      return Binary::LESS;
    case ir_t::LESS_EQ:
      return Binary::LESS_EQ;
    case ir_t::BANG_EQ:
      return Binary::NEQ;
    case ir_t::EQ_EQ:
      return Binary::EQ;
    default:
      unreachable();
    }
  }(tkn);
  auto bin = Binary{std::make_unique<ExprNode>(std::move(lhs)),
                    std::make_unique<ExprNode>(std::move(rhs)), bin_t};
  return ExprNode(ExprNode::BINARY, std::move(bin));
}

auto ExprNode::make_unary(ir_t tkn, ExprNode &&un) noexcept -> ExprNode {
  auto un_t = [](ir_t tkn) {
    switch (tkn) {
    case ir_t::MINUS:
      return Unary::MINUS;
    case ir_t::BANG:
      return Unary::BANG;
    default:
      unreachable();
    }
  }(tkn);
  auto _un = Unary{std::make_unique<ExprNode>(std::move(un)), un_t};
  return ExprNode(ExprNode::UNARY, std::move(_un));
}

auto ExprNode::make_defined(string &&str) noexcept -> ExprNode {
  return ExprNode(ExprNode::DEFINED, Defined{std::move(str)});
}

auto ExprNode::eval() const -> int {
  throw std::runtime_error("Expr_Node::eval not impl");
}

auto operator<<(std::ostream &out, ExprNode const &en) noexcept
    -> std::ostream & {
  switch (en.t) {
  case ExprNode::INT:
    out << std::get<ExprNode::Integer>(en.val).i;
    break;
  case ExprNode::NUMBER:
    out << std::get<ExprNode::Number>(en.val).f;
    break;
  case ExprNode::DEFINED:
    out << "defined (" << std::get<ExprNode::Defined>(en.val).str << ")";
    break;
  case ExprNode::CHARLIT:
    out << std::get<ExprNode::CharLit>(en.val).str;
    break;
  case ExprNode::BINARY: {
    auto &bin = std::get<ExprNode::Binary>(en.val);
    switch (bin.t) {
    case ExprNode::Binary::PLUS:
      out << '+';
      break;
    case ExprNode::Binary::MINUS:
      out << '-';
      break;
    case ExprNode::Binary::TIMES:
      out << '*';
      break;
    case ExprNode::Binary::DIVIDE:
      out << '/';
      break;
    case ExprNode::Binary::GREATER:
      out << '>';
      break;
    case ExprNode::Binary::GREATER_EQ:
      out << '>' << '=';
      break;
    case ExprNode::Binary::LESS:
      out << '<';
      break;
    case ExprNode::Binary::LESS_EQ:
      out << '<' << '=';
      break;
    case ExprNode::Binary::NEQ:
      out << '!' << '=';
      break;
    case ExprNode::Binary::EQ:
      out << '=' << '=';
      break;
    }
    out << *bin.lhs << ' ' << *bin.rhs;
  } break;
  case ExprNode::UNARY: {
    auto &un = std::get<ExprNode::Unary>(en.val);
    switch (un.t) {
    case ExprNode::Unary::BANG:
      out << '!';
      break;
    case ExprNode::Unary::MINUS:
      out << '-';
      break;
    }
    out << *un.un;
  } break;
  case ExprNode::NONE:
    break;
  }
  return out;
}

Ast::Ast() { nodes.reserve(20); }

auto AstPrinter::visit_if(IfNode &i) -> void {
  out << get_indents() << "(if (" << i.condition << ")\n";
  ++depth;
  for (auto &&thens : i.then_branch) {
    thens->accept(*this);
  }
  for (auto &&elifs : i.elif_branches) {
    elifs->accept(*this);
  }
  if (i.else_branch != nullptr) {
    i.else_branch->accept(*this);
  }
  --depth;
  out << get_indents() << ")\n";
}

auto AstPrinter::visit_ifdef(IfDefNode &i) -> void {
  out << get_indents() << "(ifdef (" << i.macro << ")\n";
  ++depth;
  for (auto &&thens : i.then_branch) {
    thens->accept(*this);
  }
  for (auto &&elifs : i.elif_branches) {
    elifs->accept(*this);
  }
  if (i.else_branch != nullptr) {
    i.else_branch->accept(*this);
  }
  --depth;
  out << get_indents() << ")\n";
}

auto AstPrinter::visit_ifndef(IfNDefNode &i) -> void {
  out << get_indents() << "(ifndef (" << i.macro << ")\n";
  ++depth;
  for (auto &&thens : i.then_branch) {
    thens->accept(*this);
  }
  for (auto &&elifs : i.elif_branches) {
    elifs->accept(*this);
  }
  if (i.else_branch != nullptr) {
    i.else_branch->accept(*this);
  }
  --depth;
  out << get_indents() << ")\n";
}

auto AstPrinter::visit_elif(ElifNode &) -> void {
  throw std::runtime_error(std::format("{} not impl", __PRETTY_FUNCTION__));
}

auto AstPrinter::visit_else(ElseNode &e) -> void {
  out << get_indents() << "(else (\n";
  ++depth;
  for (auto &&elses : e.stmts) {
    elses->accept(*this);
  }
  --depth;
  out << get_indents() << ")\n";
}

auto AstPrinter::visit_global_include(GlobalIncludeNode &global) -> void {
  out << get_indents() << "(include global (" << global.path << "))\n";
}

auto AstPrinter::visit_local_include(LocalIncludeNode &local) -> void {
  out << get_indents() << "(include local (" << local.path << "))\n";
}

auto AstPrinter::visit_define(DefineNode &) -> void {
  throw std::runtime_error(std::format("{} not impl", __PRETTY_FUNCTION__));
}

auto AstPrinter::visit_define_func(DefineFuncNode &) -> void {
  throw std::runtime_error(std::format("{} not impl", __PRETTY_FUNCTION__));
}

auto AstPrinter::visit_undef(UndefNode &) -> void {
  throw std::runtime_error(std::format("{} not impl", __PRETTY_FUNCTION__));
}

auto Exception::what() const noexcept -> string {
  return std::format("[{}]", message);
}

auto Interpreter::interpret(FixedString const &file) -> vector<fs::path> {
  auto ast = Lexer::lex(file).ast();
  auto vec = vector<fs::path>();
#ifdef DEBUG
  auto ast_p = AstPrinter(std::cout);
  ast_p.print(ast);
#endif // DEBUG
#if 0
  auto includer = AstIncluder(vec);
  ast.accept(includer);
#endif
  return vec;
}
} // namespace pp
} // namespace luamake
