#include "luamake_pre_ir.hpp"
#include "common.hpp"
#include "luamake_string_manip.hpp"
#include "luamake_strings.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <format>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unistd.h>
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

// TODO: refactor this, omg who fucking wrote this shit
enum class delims : size_t {
  LEXEME,
  BINARY_FAIL,
  ALLOWED_DECIMAL,
  ALLOWED_HEX,
  ALLOWED_OCTAL,
  ALLOWED_BINARY,
  ALPHA_SANS_HEX,
  HEX_SANS_DIGITS,
};

auto constexpr delims_list = std::array<string_view, 9>{{
    string_view{" \t\n\r(){}[]+-*/<>=#"},                    // LEXEME
    string_view{"23456789abcdefABCDEF"},                     // BINARY_FAIL
    string_view{"0123456789"},                               // ALLOWED_DECIMAL
    string_view{"0123456789abcdefABCDEF"},                   // ALLOWED_HEX
    string_view{"01234567"},                                 // ALLOWED_OCTAL
    string_view{"01"},                                       // ALLOWED_BINARY
    string_view{"ghijklmnopqrstuvwxyzGHIJKLMNOPQRSTUVWXYZ"}, // ALPHA_SANS_HEX
    string_view{"abcdefABCDEF"}                              // HEX_SANS_DIGITS
}};
auto constexpr delims_at(delims &&del) -> string_view {
  return delims_list[static_cast<std::underlying_type_t<delims>>(del)];
}

// TODO: extract this function out so we can use it in the ExprNode::eval
// function
auto is_defined(string const &str,
                std::unordered_map<std::string, Macro> const &macros,
                std::unordered_set<std::string> const &def_macros) noexcept
    -> bool {
  return macros.find(str) != macros.end()           ? true
         : def_macros.find(str) != def_macros.end() ? true
                                                    : false;
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
  LANGLE,
  RANGLE,
  QUOTE,
  LPAREN,
  RPAREN,
  MACRO, // this is used when lexing, we leave the parsing to later
  LIT_STRING,
  LEXEME,
};

constexpr auto to_string(ir_t) -> std::string_view;

// TODO: pack this even more, have something like #if node mean that there's
// also a string in the lexemes array to be read that corresponds to that #if,
// etc
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
  static auto lex(std::string_view const) -> Lexer;

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
#if 0
  auto expr(string_view const, size_t) -> size_t;
#endif

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

  auto produce_macro(string_view const, size_t) -> size_t;
  /**
   * @throws
   */
  auto expect(size_t, ir_t, string_view = "") -> void;

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
struct ExprNode final {
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
    enum class Binary_t {
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
    using enum Binary_t;
    std::unique_ptr<ExprNode> lhs;
    std::unique_ptr<ExprNode> rhs;
    Binary_t t;
  };
  struct Unary final {
    enum class Unary_t { BANG, MINUS };
    using enum Unary_t;
    std::unique_ptr<ExprNode> un;
    Unary_t t;
  };
  enum class Expr_t {
    INT,
    NUMBER,
    DEFINED,
    CHARLIT,
    BINARY,
    UNARY,
    NONE,
  } t;
  using enum Expr_t;
  using Value =
      std::variant<Integer, Number, Defined, CharLit, Binary, Unary, void *>;
  Value val;
  ExprNode() noexcept : t(NONE), val((void *)nullptr) {}
  ExprNode(Expr_t &&type, Value &&val) noexcept
      : t(type), val(std::move(val)) {}
  ExprNode(ExprNode &&) = default;
  ExprNode &operator=(ExprNode &&) = default;

  static auto make_defined(string &&) noexcept -> ExprNode;

  ExprNode(ExprNode const &) = delete;
  ExprNode &operator=(ExprNode const &) = delete;

  static auto constexpr readable_type(Expr_t) noexcept -> std::string_view;
  /**
   * @throws Exception
   * (if a float is found)
   */
  static auto eval(string_view const,
                   std::unordered_map<std::string, Macro> const &macros,
                   std::unordered_set<std::string> const &def_macros) -> int;

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
  IfNode(string &&condition, vector<ptr<AstNode>> &&then_branch,
         vector<ptr<ElifNode>> &&elif_branches,
         ptr<ElseNode> &&else_branch) noexcept
      : condition(std::move(condition)), then_branch(std::move(then_branch)),
        elif_branches(std::move(elif_branches)),
        else_branch(std::move(else_branch)) {}
  ~IfNode() final = default;
  auto accept(AstVisitor &) -> void final;

  string condition;
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
  ElifNode(string &&condition, vector<ptr<AstNode>> &&then_branch) noexcept
      : condition(std::move(condition)), then_branch(std::move(then_branch)) {}
  ~ElifNode() final = default;
  auto accept(AstVisitor &) -> void final;

  string condition;
  vector<ptr<AstNode>> then_branch;
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

// TODO: there is a bug where an extraneous MACRO tkn is being pushed back,
// causing this #define node to be treated like it has a value, as opposed to
// just being a #define MACRO
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
  DefineFuncNode(string &&name, vector<string> &&parameters,
                 string &&body) noexcept
      : name(name), parameters(parameters), body(body) {}
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

struct PragmaNode final : AstNode {
  PragmaNode() noexcept {}
  ~PragmaNode() final = default;
  auto accept(AstVisitor &visitor) -> void final;
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

struct Expressions final {
  enum class expr_t {
    LPAREN,
    RPAREN,
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
    // values
    LIT_CHAR,
    LIT_DEC,
    LIT_HEX,
    LIT_OCT,
    LIT_BIN,
    LIT_FLOAT,
    MACRO,
  };
  using enum expr_t;

  static auto constexpr to_string(expr_t t) noexcept -> string_view {
    switch (t) {
    case LPAREN:
      return string_view{"LPAREN"};
    case RPAREN:
      return string_view{"RPAREN"};
    case AND:
      return string_view{"AND"};
    case OR:
      return string_view{"OR"};
    case BIT_AND:
      return string_view{"BIT_AND"};
    case BIT_OR:
      return string_view{"BIT_OR"};
    case DEFINED:
      return string_view{"DEFINED"};
    case LESS:
      return string_view{"LESS"};
    case LESS_EQ:
      return string_view{"LESS_EQ"};
    case GREATER:
      return string_view{"GREATER"};
    case GREATER_EQ:
      return string_view{"GREATER_EQ"};
    case STRINGIZING:
      return string_view{"STRINGIZING"};
    case CONCAT:
      return string_view{"CONCAT"};
    case PLUS:
      return string_view{"PLUS"};
    case MINUS:
      return string_view{"MINUS"};
    case STAR:
      return string_view{"STAR"};
    case SLASH:
      return string_view{"SLASH"};
    case BANG_EQ:
      return string_view{"BANG_EQ"};
    case EQ:
      return string_view{"EQ"};
    case EQ_EQ:
      return string_view{"EQ_EQ"};
    case BANG:
      return string_view{"BANG"};
    case LIT_CHAR:
      return string_view{"LIT_CHAR"};
    case LIT_DEC:
      return string_view{"LIT_DEC"};
    case LIT_HEX:
      return string_view{"LIT_HEX"};
    case LIT_OCT:
      return string_view{"LIT_OCT"};
    case LIT_BIN:
      return string_view{"LIT_BIN"};
    case LIT_FLOAT:
      return string_view{"LIT_FLOAT"};
    case MACRO:
      return string_view{"MACRO"};
    }
  }

  struct ExprLexer final {
    vector<expr_t> tkns;
    vector<string> macros;
    auto to_ast() const -> ExprNode;

    auto constexpr matching(expr_t tkn, std::initializer_list<expr_t> &&matches)
        const noexcept -> bool {
      for (auto &&t : matches) {
        if (tkn == t) {
          return true;
        }
      }
      return false;
    }

    auto equality(size_t &, size_t &) const -> ExprNode;
    auto comparison(size_t &, size_t &) const -> ExprNode;
    auto term(size_t &, size_t &) const -> ExprNode;
    auto factor(size_t &, size_t &) const -> ExprNode;
    auto unary(size_t &, size_t &) const -> ExprNode;
    auto primary(size_t &, size_t &) const -> ExprNode;

    auto expect(size_t cur_t, expr_t &&tkn) const -> void {
      if (tkns[cur_t] != tkn) {
        throw Exception(std::format("Unexpected token, expected {}, found {}",
                                    to_string(tkn), to_string(tkns[cur_t])));
      }
    }
#ifdef DEBUG
    auto display(std::ostream &) const noexcept -> std::ostream &;
#endif // DEBUG
  };

  static auto lex(string_view const) -> ExprLexer;
  static auto lex_integer(string_view const, size_t &, vector<expr_t> &,
                          vector<string> &) -> void;
  static auto eval_impl(ExprNode const &,
                        std::unordered_map<std::string, Macro> const &,
                        std::unordered_set<std::string> const &) -> int;

  static auto make_binary(expr_t, ExprNode &&, ExprNode &&) noexcept
      -> ExprNode;
  static auto make_unary(expr_t, ExprNode &&) noexcept -> ExprNode;
  static auto make_integer(expr_t, string_view) noexcept -> ExprNode;
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
  virtual auto visit_pragma(PragmaNode &) -> void = 0;
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
auto PragmaNode::accept(AstVisitor &visitor) -> void {
  return visitor.visit_pragma(*this);
}

#ifdef DEBUG
struct AstPrinter final : AstVisitor {
  std::ostream &out;
  // i really love that in c++ this is a thing you can do :)
  std::allocator_traits<std::string::allocator_type>::size_type depth;
  AstPrinter(std::ostream &out) noexcept : out(out), depth(0) {}

  auto print(Ast &ast) -> void {
    out << "AstPrinter:\n";
    for (auto &&node : ast.nodes) {
      node->accept(*this);
    }
    out << "---\n";
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
  auto visit_pragma(PragmaNode &) -> void final;
};
#endif

// TODO: rewrite this implimentation so that the vector of paths is just
// returned instead of being a part of this struct
struct AstIncluder final : AstVisitor {
  vector<fs::path> &paths;
  std::unordered_map<std::string, Macro> &macros;
  std::unordered_set<std::string> &def_macros;
  AstIncluder(vector<fs::path> &paths,
              std::unordered_map<std::string, Macro> &macros,
              std::unordered_set<std::string> &def_macros) noexcept
      : paths(paths), macros(macros), def_macros(def_macros) {}

  auto get_includes(Ast &ast) -> void {
    for (auto &&node : ast.nodes) {
      node->accept(*this);
    }
  }

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
  auto visit_pragma(PragmaNode &) -> void final;
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
  case ir_t::LANGLE:
    return std::string_view("LANGLE");
  case ir_t::RANGLE:
    return std::string_view("RANGLE");
  case ir_t::QUOTE:
    return std::string_view("QUOTE");
  case ir_t::LPAREN:
    return std::string_view("LPAREN");
  case ir_t::RPAREN:
    return std::string_view("RPAREN");
  case ir_t::MACRO:
    return std::string_view("MACRO");
  case ir_t::LEXEME:
    return std::string_view("LEXEME");
  case ir_t::LIT_STRING:
    return std::string_view("LIT_STRING");
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
auto Lexer::lex(std::string_view const file) -> Lexer {
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
    {string_view{"#undef"}, ir_t::UNDEF},
    {string_view{"#pragma"}, ir_t::PRAGMA}
  }};
  auto constexpr chars_of_interest = string_view{"#/\""};
  // clang-format on
  auto lex = Lexer();
  lex.types.reserve(64);
  lex.lexemes.reserve(10);

  // this can be removed idk i'm just leaving it here rn because i don't want to
  // do a big refactor of this system rn
  auto const fcontent = file;
  auto const start = fcontent.begin();
  for (auto i = size_t{}; i < file.size();) {
    switch (fcontent[i]) {
    case '#': {
      auto end = luamake::skip_until(std::string_view(" \t\r\n"), fcontent, i);

      auto const hash_keyword = string_view{start + i, start + end};
      i = end;
      auto const keyword = keywords.find(hash_keyword);
      if (keyword == keywords.end()) {
        // continue to next character of interest
        i = luamake::skip_until(chars_of_interest, fcontent, i);
        throw Exception(
            std::format("Hash keyword [{}], is not implimented", hash_keyword));
      }

      switch (keyword->second) {
      case ir_t::INCLUDE: {
        if (i >= file.size())
          throw Exception(string("Unable to parse include parameter"));

        i = skip_ws(fcontent, i);

        if (i >= file.size())
          throw Exception(string("Unable to parse include parameter"));

        lex.types.push_back(ir_t::INCLUDE);
        switch (fcontent[i]) {
        case '<': {
          lex.types.push_back(ir_t::LANGLE);

          end = luamake::skip_until('>', fcontent, i + 1);

          if (!(end < file.size())) {
            throw Exception(string("Non terminated global include"));
          }

          lex.types.push_back(ir_t::LIT_STRING);
          lex.lexemes.push_back(string(start + i + 1, start + end));
          i = end + 1;
          lex.types.push_back(ir_t::RANGLE);
        } break;
        case '"': {
          lex.types.push_back(ir_t::QUOTE);

          end = luamake::skip_until('"', fcontent, i + 1);

          if (!(end < file.size())) {
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
        i = skip_ws(fcontent, i);
        end = luamake::skip_until(std::string_view(" \t\n\r"), fcontent, i + 1);
        lex.push_lexeme(start + i, start + end);
      } break;
      case ir_t::IFNDEF: {
        lex.types.push_back(ir_t::IFNDEF);
        i = skip_ws(fcontent, i);
        end = luamake::skip_until(std::string_view(" \t\n\r"), fcontent, i + 1);
        lex.push_lexeme(start + i, start + end);
      } break;
      case ir_t::DEFINE: {
        lex.types.push_back(ir_t::DEFINE);
        i = skip_ws(fcontent, i);
        end =
            luamake::skip_until(std::string_view(" (\t\n\r"), fcontent, i + 1);
        lex.push_lexeme(string(start + i, start + end));
        i = end;
        switch (fcontent[i]) {
        case '(': {
          lex.types.push_back(ir_t::LPAREN);
          // we don't need to skip any ws, because otherwise the macro wouldn't
          // be a function like macro
          i = lex.parse_define_args(fcontent, skip_ws(fcontent, i + 1));
          if (fcontent[i] != ')') {
            throw Exception(std::format("Expected closing ')' when parsing "
                                        "function macro arguments"));
          }
          lex.types.push_back(ir_t::RPAREN);
          // this isn't technically needed, but every example (including those
          // from the official gnu documentation) skip any preceeding ws
          i = luamake::skip_while(std::string_view{" \t"}, fcontent, i + 1);
          i = lex.produce_macro(fcontent, i);
        } break;
          // this is a bandaid solution, we need to do something to determine if
          // this is just defining a macro, or if we're actually defining a
          // macro with some token stream as a value
        case '\t':
          [[fallthrough]];
        case ' ': {
          i = skip_ws(fcontent, i);
          i = lex.produce_macro(fcontent, i);
        } break;
        default: {
          i = skip_ws(fcontent, i);
        } break;
        }
      } break;
      case ir_t::UNDEF: {
        lex.types.push_back(ir_t::UNDEF);
        i = skip_ws(fcontent, i);
        end = luamake::skip_until(std::string_view(" \t\n\r"), fcontent, i + 1);
        lex.push_macro(start + i, start + end);
      } break;
      case ir_t::IF:
        lex.types.push_back(ir_t::IF);
        i = skip_ws(fcontent, i);
        i = lex.produce_macro(fcontent, i);
        break;
      case ir_t::PRAGMA:
        lex.types.push_back(ir_t::PRAGMA);
        i = skip_ws(fcontent, i);
        end = luamake::skip_until(std::string_view(" \t\n\r"), fcontent, i + 1);
        lex.push_lexeme(start + i, start + end);
        break;
      case ir_t::ENDIF:
        lex.types.push_back(ir_t::ENDIF);
        break;
      case ir_t::ELIF:
        lex.types.push_back(ir_t::ELIF);
        i = skip_ws(fcontent, i);
        i = lex.produce_macro(fcontent, i);
        break;
      case ir_t::ELSE:
        lex.types.push_back(ir_t::ELSE);
        i = skip_ws(fcontent, i);
        break;
      default:
#ifdef DEBUG
        std::cerr << WARNING "Unknown ir_t preprocessor directive ["
                  << to_string(keyword->second) << "]" NORMAL << '\n';
#endif // DEBUG
        lex.types.push_back(keyword->second);
        break;
      }
    } break;
    case '/': {
      if (!(i + 1 < file.size())) {
        throw Exception(string("'/' found at end of file"));
      }
      ++i;
      switch (fcontent[i]) {
      case '/': { // skip until \n
        i = luamake::skip_until('\n', fcontent, i + 1);
      } break;
      case '*': { // skip until */
        // TODO: check this code, there might be an issue if the file
        // ends with a multi line comment, i.e. */ at the end of the file
        i = skip_until_close_multicomment(fcontent, i + 1);
        if (i == file.size())
          throw Exception(string("Non terminated multi line comment"));
      } break;
      default: // probably just an op /
        i = luamake::skip_until(std::string_view("#/\""), fcontent, i + 1);
        break;
      }
    } break;
    case '"': {
      i = luamake::skip_until('"', fcontent, i + 1) + 1;
      if (!(i < file.size())) {
        throw Exception(string("Non terminated string"));
      }
    } break;
    default:
      i = luamake::skip_until(std::string_view("#/\""), fcontent, i + 1);
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

auto Lexer::produce_macro(string_view const buf, size_t i) -> size_t {
  auto constexpr is_ws = [](char const ch) -> bool {
    switch (ch) {
    case ' ':
      [[fallthrough]];
    case '\t':
      [[fallthrough]];
    case '\r':
      [[fallthrough]];
    case '\n':
      return true;
    default:
      return false;
    }
  };
  auto constexpr switch_chars = std::string_view{"\\\n/"};
  auto macro = std::string();
  auto start = i;
  auto looping = true;
  while (i < buf.size() && looping) {
    auto const ch = buf[i];
    switch (ch) {
    case '\\':
      macro.append(std::string_view{buf.begin() + start, buf.begin() + i - 1});
      ++i;
      if (i < buf.size() && buf[i] == '\n')
        ++i;
      start = i;
      break;
    case '/':
      if (i + 1 < buf.size() && buf[i + 1] == '/') {
        auto end = i - 1;
        while (is_ws(buf[end])) {
          --end;
        }
        macro.append(
            std::string_view{buf.begin() + start, buf.begin() + end + 1});
        looping = false;
      } else {
        i = luamake::skip_until(switch_chars, buf, i + 1);
      }
      break;
    case '\n':
      macro.append(std::string_view{buf.begin() + start, buf.begin() + i});
      looping = false;
      break;
    default:
      i = luamake::skip_until(switch_chars, buf, i);
      break;
    }
  }
  push_macro(macro);
  // the only way to break out of the loop is to hit a '\n' char, but we don't
  // want to include that in the string, we do want to skip over it though so we
  // add 1 here
  return i + 1;
}

// TODO: add bounds checking
auto Lexer::parse_define_args(string_view const fcontent, size_t i) -> size_t {
  auto constexpr switch_chars = std::string_view{"),. \t\r\n"};
  while (true) {
    switch (fcontent[i]) {
    case ')':
      return i;
    case '.':
      throw std::runtime_error("Variatic macros are not currently supported");
    case ',':
      i = skip_ws(fcontent, i + 1);
      break;
    case ' ':
      [[fallthrough]];
    case '\t':
      [[fallthrough]];
    case '\n':
      [[fallthrough]];
    case '\r':
      throw std::runtime_error(
          "Unexpected character while parsing function like macro. Invalid "
          "spacing between parameters.");
    default: {
      auto const end = luamake::skip_until(switch_chars, fcontent, i + 1);
      push_lexeme(fcontent.data() + i, fcontent.data() + end);
      i = end;
      break;
    }
    }
  }
  unreachable();
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

  expect(cur_t, ir_t::MACRO);
  ++cur_t;
  auto expr = lexemes[cur_lex++];
  auto then_branch = vector<ptr<AstNode>>();
  auto elif_branches = vector<ptr<ElifNode>>();
  auto else_branch = ptr<ElseNode>(nullptr);
  enum class FoundEnd {
    none,
    elif,
    _else,
    endif,
  } cur = FoundEnd::none;
  auto const determine_state = [this](auto const cur_t) {
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
  };
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
    case ir_t::PRAGMA: {
      auto res = handle_pragma(cur_t, cur_lex);
      if (res)
        then_branch.push_back(std::move(res));
    } break;
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
      while (cur_t < types.size()) {
        if (types[cur_t] == ir_t::ELSE || types[cur_t] == ir_t::ENDIF)
          break;
        elif_branches.push_back(handle_elif(cur_t, cur_lex));
      }
      cur = determine_state(cur_t);
      break;
    case FoundEnd::_else:
      if (else_branch != nullptr) {
        throw Exception("Found multiple #else directives attached to a single "
                        "#ifdef directive");
      }
      else_branch = handle_else(cur_t, cur_lex);
      cur = determine_state(cur_t);
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
  auto const determine_state = [this](auto const cur_t) {
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
  };
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
    case ir_t::PRAGMA: {
      auto res = handle_pragma(cur_t, cur_lex);
      if (res)
        then_branch.push_back(std::move(res));
    } break;
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
      cur = determine_state(cur_t);
      break;
    case FoundEnd::_else:
      if (else_branch != nullptr) {
        throw Exception("Found multiple #else directives attached to a single "
                        "#ifdef directive");
      }
      else_branch = handle_else(cur_t, cur_lex);
      cur = determine_state(cur_t);
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
    case ir_t::PRAGMA: {
      auto res = handle_pragma(cur_t, cur_lex);
      if (res)
        then_branch.push_back(std::move(res));
    } break;
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
}

auto Lexer::handle_define(size_t &cur_t, size_t &cur_lex)
    -> std::unique_ptr<AstNode> {
  ++cur_t;
  expect(cur_t, ir_t::LEXEME);
  auto lex = lexemes[cur_lex++];
  ++cur_t;

  switch (types[cur_t]) {
  case ir_t::LPAREN: {
    auto parameters = vector<string>();
    ++cur_t;
    while (types[cur_t] == ir_t::LEXEME) {
      parameters.push_back(lexemes[cur_lex++]);
      ++cur_t;
    }
    expect(cur_t, ir_t::RPAREN);
    ++cur_t;
    if (types[cur_t] != ir_t::MACRO) {
      throw std::runtime_error("Expecting macro, for the function body");
    }
    ++cur_t;
    auto body = lexemes[cur_lex++];
    return std::make_unique<DefineFuncNode>(
        std::move(lex), std::move(parameters), std::move(body));
  } break;
  case ir_t::MACRO: {
    auto macro = lexemes[cur_lex++];
    ++cur_t;
    return std::make_unique<DefineNode>(std::move(lex), std::move(macro));
  } break;
  default:
    return std::make_unique<DefineNode>(std::move(lex));
  }
}

auto Lexer::handle_undef(size_t &cur_t, size_t &cur_lex)
    -> std::unique_ptr<AstNode> {
  ++cur_t;
  if (types[cur_t] != ir_t::MACRO) {
    throw Exception(
        std::format("Expected macro in #undef preprocessor directive"));
  }
  ++cur_t;
  return std::make_unique<UndefNode>(lexemes[cur_lex++]);
}

auto Lexer::handle_include(size_t &cur_t, size_t &cur_lex)
    -> std::unique_ptr<AstNode> {
  ++cur_t;
  switch (types[cur_t]) {
  case ir_t::LANGLE: {
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

auto Lexer::handle_pragma(size_t &cur_t, size_t &cur_lex)
    -> std::unique_ptr<AstNode> {
  ++cur_t;
  if (types[cur_t] != ir_t::LEXEME)
    throw Exception(
        std::format("Expected lexeme in #pragma preprocessor directive"));
  ++cur_t;
  if (lexemes[cur_lex] != "once") {
    // ignore all pragma statements that aren't #pragma once, might need to
    // rethink this
    ++cur_t;
    ++cur_lex;
    return nullptr;
  }
  return std::make_unique<PragmaNode>();
}

auto Lexer::handle_elif(size_t &cur_t, size_t &cur_lex) -> ptr<ElifNode> {
  ++cur_t;
  expect(cur_t, ir_t::MACRO);
  auto condition = lexemes[cur_lex++];
  ++cur_t;

  auto then_branch = vector<ptr<AstNode>>();
  while (cur_t < types.size()) {
    if (types[cur_t] == ir_t::ELSE || types[cur_t] == ir_t::ELIF ||
        types[cur_t] == ir_t::ENDIF)
      break;
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
    case ir_t::PRAGMA: {
      auto res = handle_pragma(cur_t, cur_lex);
      if (res)
        then_branch.push_back(std::move(res));
    } break;
    case ir_t::ELIF:
      [[fallthrough]];
    case ir_t::ENDIF:
      [[fallthrough]];
    case ir_t::ELSE:
      unreachable();
      break;
    default:
      throw Exception(
          std::format("Unexpected token [{}] found in top level scope.",
                      to_string(types[cur_t])));
    }
  }

  return std::make_unique<ElifNode>(std::move(condition),
                                    std::move(then_branch));
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
    case ir_t::PRAGMA: {
      auto prag = handle_pragma(cur_t, cur_lex);
      if (prag)
        res.push_back(std::move(prag));
    } break;
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

auto Lexer::expect(size_t cur_t, ir_t tkn, string_view calling_func) -> void {
  enum class FailReason {
    OOB,
    Unexpected,
    ok
  } reason = cur_t >= types.size() ? FailReason::OOB
             : types[cur_t] != tkn ? FailReason::Unexpected
                                   : FailReason::ok;
  switch (reason) {
  [[likely]]
  case FailReason::ok:
    return;
  case FailReason::OOB:
    if (calling_func != "") {
      throw Exception(std::format(
          "Attempting to index out of bounds of Lexer::types "
          "array, looking for token [{}], from calling function = [{}]",
          to_string(tkn), calling_func));
    } else {
      throw Exception(
          std::format("Attempting to index out of bounds of Lexer::types "
                      "array, looking for token [{}]",
                      to_string(tkn)));
    }
    return;
  case FailReason::Unexpected:
    if (calling_func != "") {
      throw Exception(std::format("Unexpected token, expected {}, found {}, "
                                  "from calling function = [{}]",
                                  to_string(tkn), to_string(types[cur_t]),
                                  calling_func));
    } else {
      throw Exception(std::format("Unexpected token, expected {}, found {}",
                                  to_string(tkn), to_string(types[cur_t])));
    }
    return;
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

auto ExprNode::make_defined(string &&str) noexcept -> ExprNode {
  return ExprNode(ExprNode::DEFINED, Defined{std::move(str)});
}

auto ExprNode::eval(string_view const expr,
                    std::unordered_map<std::string, Macro> const &macros,
                    std::unordered_set<std::string> const &def_macros) -> int {
  auto const expr_ast = Expressions::lex(expr);
  return Expressions::eval_impl(expr_ast.to_ast(), macros, def_macros);
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

auto Expressions::ExprLexer::to_ast() const -> ExprNode {
  auto cur_t = size_t{};
  auto cur_lex = size_t{};
  return equality(cur_t, cur_lex);
}

auto Expressions::ExprLexer::equality(size_t &cur_t, size_t &cur_lex) const
    -> ExprNode {
  auto lhs = comparison(cur_t, cur_lex);

  while (cur_t < tkns.size() && matching(tkns[cur_t], {BANG_EQ, EQ_EQ})) {
    auto const tkn = tkns[cur_t++];
    auto rhs = comparison(cur_t, cur_lex);

    lhs = make_binary(tkn, std::move(lhs), std::move(rhs));
  }

  return lhs;
}

auto Expressions::ExprLexer::comparison(size_t &cur_t, size_t &cur_lex) const
    -> ExprNode {
  auto lhs = term(cur_t, cur_lex);

  while (cur_t < tkns.size() &&
         matching(tkns[cur_t], {LESS, LESS_EQ, GREATER, GREATER_EQ})) {
    auto const tkn = tkns[cur_t++];
    auto rhs = term(cur_t, cur_lex);

    lhs = make_binary(tkn, std::move(lhs), std::move(rhs));
  }

  return lhs;
}

auto Expressions::ExprLexer::term(size_t &cur_t, size_t &cur_lex) const
    -> ExprNode {
  auto lhs = factor(cur_t, cur_lex);

  while (cur_t < tkns.size() && matching(tkns[cur_t], {PLUS, MINUS})) {
    auto const tkn = tkns[cur_t++];
    auto rhs = factor(cur_t, cur_lex);

    lhs = make_binary(tkn, std::move(lhs), std::move(rhs));
  }

  return lhs;
}

auto Expressions::ExprLexer::factor(size_t &cur_t, size_t &cur_lex) const
    -> ExprNode {
  auto lhs = unary(cur_t, cur_lex);

  while (cur_t < tkns.size() && matching(tkns[cur_t], {STAR, SLASH})) {
    auto const tkn = tkns[cur_t++];
    auto rhs = unary(cur_t, cur_lex);

    lhs = make_binary(tkn, std::move(lhs), std::move(rhs));
  }

  return lhs;
}

auto Expressions::ExprLexer::unary(size_t &cur_t, size_t &cur_lex) const
    -> ExprNode {
  if (cur_t < tkns.size() && matching(tkns[cur_t], {BANG, MINUS})) {
    auto const tkn = tkns[cur_t++];
    auto un = unary(cur_t, cur_lex);
    return make_unary(tkn, std::move(un));
  }
  return primary(cur_t, cur_lex);
}

auto Expressions::ExprLexer::primary(size_t &cur_t, size_t &cur_lex) const
    -> ExprNode {
  // TODO
  switch (tkns[cur_t]) {
  case MACRO:
    break;
  case LIT_CHAR:
    break;
  case LIT_DEC:
    return make_integer(expr_t::LIT_DEC, macros[cur_lex++]);
  case LIT_HEX:
    break;
  case LIT_OCT:
    break;
  case LIT_BIN:
    break;
  case LIT_FLOAT:
    break;
  case LPAREN: {
    ++cur_t;
    if (tkns[cur_t] != RPAREN) {
      throw Exception(std::format("While parsing a grouping expression, "
                                  "expected a ')' to wrap the expression"));
    }
  } break;
  case DEFINED: {
    ++cur_t;
    auto lex = string();
    if (tkns[cur_t] == LPAREN) {
      ++cur_t;
      expect(cur_t, MACRO);
      ++cur_t;
      lex = macros[cur_lex++];
      expect(cur_t, RPAREN);
      ++cur_t;
    } else {
      expect(cur_t, MACRO);
      ++cur_t;
      lex = macros[cur_lex++];
    }
    return ExprNode::make_defined(std::move(lex));
  } break;

  default:
    throw Exception(
        std::format("Unexpected token [{}] found while parsing an expression",
                    to_string(tkns[cur_t])));
  }
  throw Exception(std::format("{} not impl", __PRETTY_FUNCTION__));
}

#ifdef DEBUG
auto Expressions::ExprLexer::display(std::ostream &out) const noexcept
    -> std::ostream & {
  out << "Tokens:\n\t";
  for (auto const &type : tkns) {
    out << '[' << to_string(type) << ']';
  }
  out << '\n';
  out << "Macros:\n\t";
  for (auto const &macro : macros) {
    out << '[' << macro << ']';
  }
  out << '\n';
  return out;
}
#endif // DEBUG

auto Expressions::lex(string_view const str) -> ExprLexer {
  auto constexpr defined_str = string_view{"defined"};
  auto tkns = vector<expr_t>();
  auto macros = vector<string>();
  for (auto i = size_t{}; i < str.size();) {
    // TODO: probably add a macro for these basic types so that we don't have to
    // write out a bunch of things every time, and so that this function can be
    // smaller
    switch (auto ch = str[i]) {
    case '+':
      tkns.push_back(PLUS);
      ++i;
      break;
    case '(':
      tkns.push_back(LPAREN);
      ++i;
      break;
    case ')':
      tkns.push_back(RPAREN);
      ++i;
      break;
    case '|': {
      ++i;
      if (i < str.size() && str[i] == '|') {
        ++i;
        tkns.push_back(OR);
      } else {
        tkns.push_back(BIT_OR);
      }
    } break;
    case '&': {
      ++i;
      if (i < str.size() && str[i] == '&') {
        ++i;
        tkns.push_back(AND);
      } else {
        tkns.push_back(BIT_AND);
      }
    } break;
    case '=': {
      ++i;
      if (i < str.size() && str[i] == '=') {
        ++i;
        tkns.push_back(EQ_EQ);
      } else {
        tkns.push_back(EQ);
      }
    } break;
    case '!': {
      ++i;
      if (i < str.size() && str[i] == '=') {
        ++i;
        tkns.push_back(BANG_EQ);
      } else {
        tkns.push_back(BANG);
      }
    } break;
    case '<': {
      ++i;
      if (i < str.size() && str[i] == '=') {
        ++i;
        tkns.push_back(LESS_EQ);
      } else {
        tkns.push_back(LESS);
      }
    } break;
    case '>': {
      ++i;
      if (i < str.size() && str[i] == '=') {
        ++i;
        tkns.push_back(GREATER_EQ);
      } else {
        tkns.push_back(GREATER);
      }
    } break;
    case '#': {
      ++i;
      if (i < str.size() && str[i] == '#') {
        ++i;
        tkns.push_back(STRINGIZING);
      } else {
        tkns.push_back(CONCAT);
      }
    } break;
    case '.':
      throw Exception(std::format("found '.' while parsing expression [{}] in "
                                  "a #if or #elif condition.",
                                  str));
    case 'd': {
      if (i + defined_str.size() < str.size() &&
          strncmp(str.data() + i, defined_str.data(), defined_str.size()) ==
              0) {
        i += defined_str.size();
        tkns.push_back(DEFINED);
      } else {
        auto const start = i;
        i = luamake::skip_until(delims_at(delims::LEXEME), str, i);
        tkns.push_back(MACRO);
        macros.push_back(string(str.data() + start, str.data() + i));
      }
    } break;
    case ' ':
      [[fallthrough]];
    case '\t':
      // idk should probably add a thing for this
      ++i;
      break;
    default: {
      if (is_digit(ch)) {
        lex_integer(str, i, tkns, macros);
      } else {
        auto const start = i;
        i = luamake::skip_until(delims_at(delims::LEXEME), str, i);
        tkns.push_back(MACRO);
        macros.push_back(string(str.data() + start, str.data() + i));
      }
    }
    }
  }
#ifdef DEBUG
  auto const lexer = ExprLexer(tkns, macros);
  lexer.display(std::cout).flush();
  return lexer;
#else
  return ExprLexer(tkns, macros);
#endif // DEBUG
}

// TODO: support integer suffixes
auto Expressions::lex_integer(string_view const str, size_t &i,
                              vector<expr_t> &tkns, vector<string> &macros)
    -> void {
  if (str[i] != '0') {
    auto const start = i;
    i = luamake::skip_while(delims_at(delims::ALLOWED_DECIMAL), str, i);
    tkns.push_back(LIT_DEC);
    macros.push_back(string(str.data() + start, str.data() + i));
    return;
  }
  // str[i] == 0
  ++i;
  if (!(i < str.size())) {
    tkns.push_back(LIT_DEC);
    macros.push_back(string(1, '0'));
    return;
  }

  // TODO: check if we're on c++14>=, bc otherwise this is supposed to
  // be an error, the same goes with "'" character
  // TODO: this is really poorly written i should really just extract
  // this out into it's own function
  enum class int_type {
    DECIMAL,
    BINARY,
    HEX,
    OCTAL
  } int_t = [&]() {
    switch (auto ch = str[i]) {
    case 'x':
      return int_type::HEX;
    case 'b':
      return int_type::BINARY;
    case 'o':
      return int_type::OCTAL;
    default:
      if (!is_digit(ch)) {
        throw Exception(std::format(
            "Found char [{}], while attempting to parse an integer", ch));
      } else {
        return int_type::DECIMAL;
      }
    }
  }();
  auto constexpr to_string = [](int_type int_t) -> string_view {
    switch (int_t) {
    case int_type::DECIMAL:
      return string_view{"DECIMAL"};
    case int_type::BINARY:
      return string_view{"BINARY"};
    case int_type::HEX:
      return string_view{"HEX"};
    case int_type::OCTAL:
      return string_view{"OCTAL"};
    }
  };

  auto constexpr is_allowed_char = [](int_type int_t, char ch) -> bool {
    switch (int_t) {
    case int_type::DECIMAL:
      return is_any_of(delims_at(delims::ALLOWED_DECIMAL), ch);
    case int_type::BINARY:
      return is_any_of(delims_at(delims::ALLOWED_BINARY), ch);
    case int_type::HEX:
      return is_any_of(delims_at(delims::ALLOWED_HEX), ch);
    case int_type::OCTAL:
      return is_any_of(delims_at(delims::ALLOWED_OCTAL), ch);
    }
  };
  auto allow_quote = true;
  while (i < str.size()) {
    if (str[i] == '\'') {
      if (!allow_quote) {
        throw Exception(
            std::format("When parsing a {} number, found two ' characters "
                        "back to back, these are treated as identifiers for a "
                        "char literal, and thus a formatting error.",
                        to_string(int_t)));
      } else {
        allow_quote = false;
        ++i;
      }
    }
    if (!is_allowed_char(int_t, str[i])) {
      throw Exception(std::format("While parsing a [{}] integer, found "
                                  "[{}], a not supported character",
                                  to_string(int_t), str[i]));
    }
    ++i;
    allow_quote = true;
  }
}

auto Expressions::eval_impl(
    ExprNode const &e, std::unordered_map<std::string, Macro> const &macros,
    std::unordered_set<std::string> const &def_macros) -> int {
  switch (e.t) {
  case ExprNode::INT:
    return static_cast<int>(std::get<ExprNode::Integer>(e.val).i);
    break;
  case ExprNode::DEFINED:
    return is_defined(std::get<ExprNode::Defined>(e.val).str, macros,
                      def_macros)
               ? 1
               : 0;
  case ExprNode::NUMBER:
    [[fallthrough]];
  case ExprNode::CHARLIT:
    throw Exception(
        std::format("While evaluating if expression found a not integer."));
  case ExprNode::BINARY: {
    auto &bin = std::get<ExprNode::Binary>(e.val);
    auto const lhs = eval_impl(*bin.lhs, macros, def_macros);
    auto const rhs = eval_impl(*bin.rhs, macros, def_macros);
    switch (bin.t) {
    case ExprNode::Binary::PLUS:
      return lhs + rhs;
    case ExprNode::Binary::MINUS:
      return lhs - rhs;
    case ExprNode::Binary::TIMES:
      return lhs * rhs;
    case ExprNode::Binary::DIVIDE:
      return lhs / rhs;
    case ExprNode::Binary::GREATER:
      return lhs > rhs ? 1 : 0;
    case ExprNode::Binary::GREATER_EQ:
      return lhs >= rhs ? 1 : 0;
    case ExprNode::Binary::LESS:
      return lhs < rhs ? 1 : 0;
    case ExprNode::Binary::LESS_EQ:
      return lhs <= rhs ? 1 : 0;
    case ExprNode::Binary::NEQ:
      return lhs != rhs ? 1 : 0;
    case ExprNode::Binary::EQ:
      return lhs == rhs ? 1 : 0;
    }
  }
  case ExprNode::UNARY: {
    auto &un = std::get<ExprNode::Unary>(e.val);
    auto const res = eval_impl(*un.un, macros, def_macros);
    switch (un.t) {
    case ExprNode::Unary::MINUS:
      return -res;
    case ExprNode::Unary::BANG:
      return !res;
    }
  }
  case ExprNode::NONE:
    throw Exception(
        std::format("Attempting to evaluate an uninitialized expression."));
  }
}

auto Expressions::make_binary(Expressions::expr_t tkn, ExprNode &&lhs,
                              ExprNode &&rhs) noexcept -> ExprNode {
  auto bin_t = [](expr_t tkn) {
    switch (tkn) {
    case PLUS:
      return ExprNode::Binary::PLUS;
    case MINUS:
      return ExprNode::Binary::MINUS;
    case SLASH:
      return ExprNode::Binary::DIVIDE;
    case STAR:
      return ExprNode::Binary::TIMES;
    case GREATER:
      return ExprNode::Binary::GREATER;
    case GREATER_EQ:
      return ExprNode::Binary::GREATER_EQ;
    case LESS:
      return ExprNode::Binary::LESS;
    case LESS_EQ:
      return ExprNode::Binary::LESS_EQ;
    case BANG_EQ:
      return ExprNode::Binary::NEQ;
    case EQ_EQ:
      return ExprNode::Binary::EQ;
    default:
      unreachable();
    }
  }(tkn);
  auto bin =
      ExprNode::Binary{std::make_unique<ExprNode>(std::move(lhs)),
                       std::make_unique<ExprNode>(std::move(rhs)), bin_t};
  return ExprNode(ExprNode::BINARY, std::move(bin));
}

auto Expressions::make_unary(Expressions::expr_t tkn, ExprNode &&un) noexcept
    -> ExprNode {
  auto un_t = [](expr_t tkn) {
    switch (tkn) {
    case MINUS:
      return ExprNode::Unary::MINUS;
    case BANG:
      return ExprNode::Unary::BANG;
    default:
      unreachable();
    }
  }(tkn);
  auto _un = ExprNode::Unary{std::make_unique<ExprNode>(std::move(un)), un_t};
  return ExprNode(ExprNode::UNARY, std::move(_un));
}

auto Expressions::make_integer(Expressions::expr_t tkn,
                               string_view str) noexcept -> ExprNode {
  auto i = [str](expr_t tkn) -> size_t {
    switch (tkn) {
    case LIT_DEC: {
      // TODO: idk i feel like i could do better but this is fine
      auto res = size_t{};
      sscanf(str.data(), "%zu", &res);
      return res;
    } break;
    case LIT_CHAR:
      [[fallthrough]];
    case LIT_HEX:
      [[fallthrough]];
    case LIT_OCT:
      [[fallthrough]];
    case LIT_BIN:
      throw Exception(std::format(
          "Parsing Expr_t [{}], is not currently implimented", to_string(tkn)));
      break;
    default:
      unreachable();
    }
  }(tkn);
  return ExprNode(ExprNode::INT, ExprNode::Integer{i});
}

#ifdef DEBUG
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

auto AstPrinter::visit_elif(ElifNode &e) -> void {
  out << get_indents() << "(elif (" << e.condition << ")\n";
  ++depth;
  for (auto &&thens : e.then_branch) {
    thens->accept(*this);
  }
  --depth;
  out << get_indents() << ")\n";
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

auto AstPrinter::visit_define(DefineNode &d) -> void {
  out << get_indents() << "(define (" << d.name;
  if (d.lexeme) {
    out << '{';
    out << d.lexeme.value();
    out << '}';
  }
  out << "))\n";
}

auto AstPrinter::visit_define_func(DefineFuncNode &f) -> void {
  out << get_indents() << "(define (" << f.name << "(";
  for (auto &&param : f.parameters) {
    out << param << ",";
  }
  out << ")";
  out << "{" << f.body << "}))\n";
}

auto AstPrinter::visit_undef(UndefNode &u) -> void {
  out << get_indents() << "(undef (" << u.name;
  out << "))\n";
}

auto AstPrinter::visit_pragma(PragmaNode &p) -> void {
  out << get_indents() << "(pragma once)\n";
}
#endif // DEBUG

auto AstIncluder::visit_if(IfNode &i) -> void {
  if (ExprNode::eval(i.condition, macros, def_macros) != 0) {
    for (auto &&thens : i.then_branch) {
      thens->accept(*this);
    }
    return;
  }
  for (auto &&elif : i.elif_branches) {
    if (ExprNode::eval(elif->condition, macros, def_macros) != 0) {
      elif->accept(*this);
      return;
    }
  }
  if (i.else_branch != nullptr) {
    i.else_branch->accept(*this);
  }
}

auto AstIncluder::visit_ifdef(IfDefNode &i) -> void {
  if (is_defined(i.macro, macros, def_macros)) {
    for (auto &&thens : i.then_branch) {
      thens->accept(*this);
    }
    return;
  }
  for (auto &&elif : i.elif_branches) {
    if (ExprNode::eval(elif->condition, macros, def_macros) != 0) {
      elif->accept(*this);
      return;
    }
  }
  if (i.else_branch != nullptr) {
    i.else_branch->accept(*this);
  }
}

auto AstIncluder::visit_ifndef(IfNDefNode &i) -> void {
  if (!is_defined(i.macro, macros, def_macros)) {
    for (auto &&thens : i.then_branch) {
      thens->accept(*this);
    }
    return;
  }
  for (auto &&elif : i.elif_branches) {
    if (ExprNode::eval(elif->condition, macros, def_macros) != 0) {
      elif->accept(*this);
      return;
    }
  }
  if (i.else_branch != nullptr) {
    i.else_branch->accept(*this);
  }
}

auto AstIncluder::visit_elif(ElifNode &e) -> void {
  for (auto &&branch : e.then_branch) {
    branch->accept(*this);
  }
}

auto AstIncluder::visit_else(ElseNode &e) -> void {
  for (auto &&elses : e.stmts) {
    elses->accept(*this);
  }
}

auto AstIncluder::visit_global_include(GlobalIncludeNode &global) -> void {
  // TODO: check that global include is in the global include path
}

auto AstIncluder::visit_local_include(LocalIncludeNode &local) -> void {
  paths.push_back(local.path);
}

auto AstIncluder::visit_define(DefineNode &d) -> void {
  if (d.lexeme) {
    macros[d.name] = d.lexeme.value();
  } else {
    def_macros.insert(d.name);
  }
}

auto AstIncluder::visit_define_func(DefineFuncNode &f) -> void {
  // when we fix how function macros are stored, we'll need to update this
  auto cur_format = [&f]() -> string {
    auto res = f.name;
    res.append("(");
    for (auto i = size_t{}; i < f.parameters.size(); ++i) {
      res.append(f.parameters[i]);
      if (i != f.parameters.size() - 1)
        res.append(",");
    }
    res.append(")");
    return res;
  }();
  macros[cur_format] = f.body;
}

auto AstIncluder::visit_undef(UndefNode &u) -> void {
  if (macros.contains(u.name)) {
    macros.erase(u.name);
  } else if (def_macros.contains(u.name)) {
    def_macros.erase(u.name);
  } else {
    //  apparently it's perfectly fine to #undef a non-existant macro, at
    //  least according to clang i should check what the docs have to say
    //  about this case
  }
}

auto AstIncluder::visit_pragma(PragmaNode &) -> void {
  return; // ? idk if there's actually anything for us to do here
}

auto Exception::what() const noexcept -> string {
  return std::format("[{}]", message);
}

auto Interpreter::interpret(std::string_view const file) -> vector<fs::path> {
  auto ast = Lexer::lex(file).ast();
  auto vec = vector<fs::path>();
#ifdef DEBUG
  auto ast_p = AstPrinter(std::cout);
  ast_p.print(ast);
#endif // DEBUG
  auto includer = AstIncluder(vec, macros, def_macros);
  includer.get_includes(ast);
#ifdef DEBUG
  std::cout << "including:\n";
  for (auto &&include : vec) {
    std::cout << "\t[" << include << "]\n";
  }
  std::cout.flush();
#endif // DEBUG
  return vec;
}

#ifdef DEBUG
auto Interpreter::dump_macros(std::ostream &out) noexcept -> void {
  out << "macros = {\n";
  for (auto &&[name, value] : macros) {
    out << name << "=" << value << ",\n";
  }
  out << "}\n";

  out << "defined_macros = ";
  out << "[" << def_macros.size() << "]{\n";
  for (auto const &name : def_macros) {
    out << name << ",\n";
  }
  out << "}\n";
}
#endif // DEBUG
} // namespace pp
} // namespace luamake
