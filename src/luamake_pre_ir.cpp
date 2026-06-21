#include "luamake_pre_ir.hpp"
#include "common.hpp"
#include "luamake_allocator.hpp"
#include "luamake_string_manip.hpp"
#include "luamake_strings.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <format>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef DEBUG_CPP
#include <ostream>
#endif // DEBUG

namespace fs = std::filesystem;

template <class T> using ptr = std::unique_ptr<T>;

using buffer_views = luamake::StringViews;

template <class T, class... Values>
concept any_of = (std::is_same_v<T, Values> || ...);

// TODO: this system is over complicated, i think that we only really need 2
// components instead of the 3 currently, a change to this would require a
// complete rearchitecure of the code, and i'm too fucking exhaused to do that
// rn, so i'll get to it in a later commit

// TODO: arena allocate this whole structure, specifically the ast stuff

// TODO: test if using lazy parsing improves performance, it would allow us to
// cut back on some memory useage, also short circuit when we come across a
// macro, also we need to look into allowing #pragma once, and what that
// semantically means

namespace luamake::pp {
namespace {
auto constexpr skip_until_close_multicomment(std::string_view const buf,
                                             size_t i) -> size_t {
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

auto constexpr delims_list = std::array<std::string_view, 9>{{
    std::string_view{" \t\n\r(){}[]+-*/<>=#"},  // LEXEME
    std::string_view{"23456789abcdefABCDEF"},   // BINARY_FAIL
    std::string_view{"0123456789"},             // ALLOWED_DECIMAL
    std::string_view{"0123456789abcdefABCDEF"}, // ALLOWED_HEX
    std::string_view{"01234567"},               // ALLOWED_OCTAL
    std::string_view{"01"},                     // ALLOWED_BINARY
    std::string_view{
        "ghijklmnopqrstuvwxyzGHIJKLMNOPQRSTUVWXYZ"}, // ALPHA_SANS_HEX
    std::string_view{"abcdefABCDEF"}                 // HEX_SANS_DIGITS
}};

auto constexpr delims_at(delims &&del) -> std::string_view {
  return delims_list[static_cast<std::underlying_type_t<delims>>(del)];
}

auto is_defined(std::string_view const str, pp::MacroMap const &macros,
                pp::StringSet const &defs) noexcept -> bool {
  return macros.find(str) != macros.end() ? true
         : defs.find(str) != defs.end()   ? true
                                          : false;
}

struct Ast;
struct AstNode;
struct ElifNode;
struct ElseNode;

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
  VARIADIC
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

  static auto lex(std::string_view const) -> Lexer;

  auto ast() -> Ast;

#ifdef DEBUG_CPP
  auto display(std::ostream &) const noexcept -> std::ostream &;
#endif // DEBUG_CPP

  Lexer() = default;

  constexpr auto matching(size_t, std::initializer_list<ir_t> &&) noexcept
      -> bool;
  auto parse_define_args(std::string_view const, size_t) -> size_t;
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

  auto produce_macro(std::string_view const, size_t) -> size_t;
  auto produce_lexeme(std::string_view const, size_t) -> size_t;
  /**
   * @throws
   */
  auto expect(size_t, ir_t, std::string_view = "") -> void;

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

// TODO: look into integer overflow, it seems like if integer overflow happens,
// everything is ignored this is based on the example
// ```
// #if 1 << 64 // integer overflow occurs
// this is never run(?)
// #endif
// ```
// TODO: also might be fucked with how we store integers, because we'll also
// have to work with negative numbers, even though i feel like i don't really
// see many negative numbers in macros
struct ExprNode final {
  struct Integer final {
    size_t i;
  };
  struct Number final {
    double f;
  };
  // NOTE: these strings are not required to be null terminated, be sure to use
  // the right apis
  struct Defined final {
    std::string_view str;
  };
  struct CharLit final {
    std::string_view str;
  };
  struct Grouping final {
    ExprNode const *expr;
  };
  struct Binary final {
    enum class Binary_t : u32 {
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
      AND,
      OR
    };
    ExprNode const *lhs;
    ExprNode const *rhs;
  };
  struct Unary final {
    enum class Unary_t : u32 { BANG, MINUS };
    ExprNode const *un;
  };
  enum class Expr_t : u32 {
    INT,
    NUMBER,
    DEFINED,
    CHARLIT,
    GROUPING,
    BINARY,
    UNARY,
    NONE,
  };
  ExprNode() noexcept;
  // factory ctors
  template <class T>
    requires any_of<std::remove_cvref_t<T>, Integer, Number, Defined, CharLit,
                    Grouping>
  static auto from(T expr) noexcept -> ExprNode {
    auto storage = std::array<u8, STORAGE_SIZE>({});
    std::memset(storage.data(), 0, STORAGE_SIZE);
    using actual_t = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<actual_t, Integer>) {
      new (storage.data()) Expr_t(Expr_t::INT);
      new (storage.data() + sizeof(size_t)) Integer(std::move(expr));
    } else if constexpr (std::is_same_v<actual_t, Number>) {
      new (storage.data()) Expr_t(Expr_t::NUMBER);
      new (storage.data() + sizeof(size_t)) Number(std::move(expr));
    } else if constexpr (std::is_same_v<actual_t, Defined> ||
                         std::is_same_v<actual_t, CharLit>) {
      if constexpr (std::is_same_v<actual_t, Defined>) {
        new (storage.data()) Expr_t(Expr_t::DEFINED);
      } else {
        new (storage.data()) Expr_t(Expr_t::CHARLIT);
      }
      if (expr.str.size() < SMALL_STRING_AMOUNT) {
#ifdef DEBUG_CPP
        std::cout << std::format("Making small string\n\tsize={}\n",
                                 expr.str.size());
#endif // DEBUG_CPP
        storage[sizeof(Expr_t)] = 0xbe;
        std::memcpy(storage.data() + sizeof(Expr_t) + 1, expr.str.data(),
                    expr.str.size());
      } else {
#ifdef DEBUG_CPP
        std::cout << std::format("Making big string\n\tsize={}\n",
                                 expr.str.size());
#endif // DEBUG_CPP
        storage[sizeof(Expr_t)] = 0xff;
        auto *size =
            new (storage.data() + sizeof(size_t)) size_t{expr.str.size()};
        // wtf am i doing
        auto *buffer_ptr =
            new (storage.data() + 2 * sizeof(size_t)) char *{new char[*size]{}};
#ifdef DEBUG_CPP
        std::cout << std::format("memory pointing at buffer={}, *buffer={}\n",
                                 (void *)buffer_ptr, (void *)*buffer_ptr);
#endif // DEBUG_CPP
        std::memcpy(*buffer_ptr, expr.str.data(), *size);
      }
    } else if constexpr (std::is_same_v<actual_t, Grouping>) {
      new (storage.data()) Expr_t(Expr_t::GROUPING);
      new (storage.data() + sizeof(size_t)) ExprNode const *(expr.expr);
    } else if constexpr (std::is_same_v<actual_t, Binary> ||
                         std::is_same_v<actual_t, Unary>) {
      unreachable();
    }
    return ExprNode{storage};
  }

  static auto from(Unary::Unary_t un_t, ExprNode *node) -> ExprNode {
    auto storage = std::array<u8, STORAGE_SIZE>({});
    std::memset(storage.data(), 0, STORAGE_SIZE);
    new (storage.data()) Expr_t(Expr_t::UNARY);
    new (storage.data() + sizeof(Expr_t)) Unary::Unary_t(un_t);
    new (storage.data() + sizeof(size_t)) ExprNode *(node);
    return ExprNode{storage};
  }

  static auto from(Binary::Binary_t bin_t, ExprNode *lhs, ExprNode *rhs)
      -> ExprNode {
    auto storage = std::array<u8, STORAGE_SIZE>({});
    std::memset(storage.data(), 0, STORAGE_SIZE);
    new (storage.data()) Expr_t(Expr_t::BINARY);
    new (storage.data() + sizeof(Expr_t)) Binary::Binary_t(bin_t);
    new (storage.data() + sizeof(size_t)) ExprNode *(lhs);
    new (storage.data() + 2 * sizeof(size_t)) ExprNode *(rhs);
    return ExprNode{storage};
  }
  // this will do the memory management of deleting the tree recursively, making
  // sure that it's raii compatable, we should probably think about switching
  // this over to some sort of arena allocation strategy, and/or switching over
  // to lazy parsing, which would probably help, because we could exit early in
  // the case that the file is already checked, and it would allow us to support
  // pragma once macros finally, and make this tool actually useful
  ~ExprNode() noexcept;

  ExprNode(ExprNode &&) noexcept;
  ExprNode &operator=(ExprNode &&) noexcept;

  // static auto make_defined(string &&) noexcept -> ExprNode;

  ExprNode(ExprNode const &) = delete;
  ExprNode &operator=(ExprNode const &) = delete;

  static auto constexpr to_string(Expr_t) noexcept -> std::string_view;
  /**
   * @throws std::runtime_error
   * (if a float is found)
   */
  static auto eval(std::string_view const, allocator::Page &,
                   pp::MacroMap const &macros, StringSet const &def_macros)
      -> int;

  auto expr_t() const noexcept -> Expr_t;
  template <class T>
    requires any_of<T, Binary::Binary_t, Unary::Unary_t, u32>
  auto meta_data() const noexcept -> T {
    // alignment
    static_assert(sizeof(T) == 4);
    auto res = T{};
    std::memcpy(&res, storage.data() + sizeof(Expr_t), sizeof(T));
    return res;
  }

  // restricted so that we're only converting the buffer into something that it
  // should be
  template <class T>
    requires any_of<std::remove_cvref_t<T>, Integer, Number, Defined, CharLit,
                    Grouping, Binary, Unary>
  auto constexpr to() const noexcept -> T {
    // TODO: it seems like we shouldn't be calling into reinterpret_cast,
    // because it's not getting all of the info it needs for the types
    using actual_t = std::remove_cvref_t<T>;
    // NOTE: we use the fact that the memory layouts for these are the same, idk
    // if that's actually a good idea but it's what we do :)
    if constexpr (std::is_same_v<actual_t, Integer> ||
                  std::is_same_v<actual_t, Number>) {
      static_assert(sizeof(Integer) == sizeof(Number));
      auto res = T{};
      std::memcpy(&res, storage.data() + sizeof(size_t), sizeof(actual_t));
      return res;
    } else if constexpr (std::is_same_v<actual_t, Defined> ||
                         std::is_same_v<actual_t, CharLit>) {
      if (storage[sizeof(Expr_t)] == 0xbe) {
        // small string
        auto size = size_t{};
        while (storage[size + sizeof(Expr_t)] != 0) {
          ++size;
        }
        return T{std::string_view{
            reinterpret_cast<char const *>(storage.data() + sizeof(Expr_t) + 1),
            size}};
      } else {
        auto size = size_t{};
        auto *buffer = static_cast<char *>(nullptr);
        std::memcpy(&size, storage.data() + sizeof(size_t), sizeof(size_t));
        std::memcpy(&buffer, storage.data() + 2 * sizeof(size_t),
                    sizeof(char *));
        return T{std::string_view{buffer, size}};
      }
    } else if constexpr (std::is_same_v<actual_t, Grouping> ||
                         std::is_same_v<actual_t, Unary>) {
      auto const *ptr = static_cast<ExprNode const *>(nullptr);
      std::memcpy(&ptr, storage.data() + sizeof(size_t),
                  sizeof(ExprNode const *));
      return T{ptr};
    } else if constexpr (std::is_same_v<actual_t, Binary>) {
      auto const *lhs = static_cast<ExprNode const *>(nullptr);
      std::memcpy(&lhs, storage.data() + sizeof(size_t),
                  sizeof(ExprNode const *));
      auto const *rhs = static_cast<ExprNode const *>(nullptr);
      std::memcpy(&rhs, storage.data() + 2 * sizeof(size_t),
                  sizeof(ExprNode const *));
      return T{lhs, rhs};
    }
  }

#ifdef DEBUG_CPP
  auto to_string() const noexcept -> std::string;
  auto display(std::ostream &) const noexcept -> std::ostream &;
#endif // DEBUG_CPP

private:
  // 24 bytes should be enough(?)
  // this should be a fine alignment(?)
  static auto constexpr STORAGE_SIZE = size_t{24};
  // -1 to hold the byte for if the string is sso
  // TODO: make the tag a u8, and add a variant tag for sso, so that we can get
  // an additional ~4 bytes for the small string
  static auto constexpr SMALL_STRING_AMOUNT = STORAGE_SIZE - sizeof(Expr_t) - 1;
  // tag    meta data
  // v      v
  // [****][****][****************]
  //              ^
  //              union of all the types
  //              (all have alignment == 8, so this allows all of them to fit
  //              here)
  // when tag == CHARLIT || DEFINED, then it will either look like the above,
  // with storage[4] == 0xff, the rest of the meta data being 0, and the union
  // being a size_t and ptr, or when the string is small enough, storage[4] ==
  // 0xbe, and the rest of the buffer is used to hold the characters
  alignas(size_t) std::array<u8, STORAGE_SIZE> storage;

  constexpr explicit ExprNode(std::array<u8, STORAGE_SIZE> storage) noexcept
      : storage(std::move(storage)) {}
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
  IfNode(std::string &&condition, std::vector<ptr<AstNode>> &&then_branch,
         std::vector<ptr<ElifNode>> &&elif_branches,
         ptr<ElseNode> &&else_branch) noexcept
      : condition(std::move(condition)), then_branch(std::move(then_branch)),
        elif_branches(std::move(elif_branches)),
        else_branch(std::move(else_branch)) {}
  ~IfNode() final = default;
  auto accept(AstVisitor &) -> void final;

  std::string condition;
  std::vector<ptr<AstNode>> then_branch;
  std::vector<ptr<ElifNode>> elif_branches;
  ptr<ElseNode> else_branch;
};

struct IfDefNode final : AstNode {
  IfDefNode(std::string &&str, std::vector<ptr<AstNode>> &&then_branch,
            std::vector<ptr<ElifNode>> &&elif_branches,
            ptr<ElseNode> &&else_branch) noexcept
      : macro(std::move(str)), then_branch(std::move(then_branch)),
        elif_branches(std::move(elif_branches)),
        else_branch(std::move(else_branch)) {}
  ~IfDefNode() final = default;
  auto accept(AstVisitor &) -> void final;

  std::string macro;
  std::vector<ptr<AstNode>> then_branch;
  std::vector<ptr<ElifNode>> elif_branches;
  ptr<ElseNode> else_branch;
};

struct IfNDefNode final : AstNode {
  IfNDefNode(std::string &&str, std::vector<ptr<AstNode>> &&then_branch,
             std::vector<ptr<ElifNode>> &&elif_branches,
             ptr<ElseNode> &&else_branch) noexcept
      : macro(std::move(str)), then_branch(std::move(then_branch)),
        elif_branches(std::move(elif_branches)),
        else_branch(std::move(else_branch)) {}
  ~IfNDefNode() final = default;
  auto accept(AstVisitor &) -> void final;

  std::string macro;
  std::vector<ptr<AstNode>> then_branch;
  std::vector<ptr<ElifNode>> elif_branches;
  ptr<ElseNode> else_branch;
};

struct ElifNode final : AstNode {
  ElifNode(std::string &&condition,
           std::vector<ptr<AstNode>> &&then_branch) noexcept
      : condition(std::move(condition)), then_branch(std::move(then_branch)) {}
  ~ElifNode() final = default;
  auto accept(AstVisitor &) -> void final;

  std::string condition;
  std::vector<ptr<AstNode>> then_branch;
};

struct ElseNode final : AstNode {
  ElseNode(std::vector<ptr<AstNode>> &&stmts) noexcept
      : stmts(std::move(stmts)) {}
  ~ElseNode() final = default;
  auto accept(AstVisitor &) -> void final;

  std::vector<ptr<AstNode>> stmts;
};

struct GlobalIncludeNode final : AstNode {
  GlobalIncludeNode(std::string const &str) noexcept : path(str) {}
  ~GlobalIncludeNode() final = default;
  auto accept(AstVisitor &) -> void final;

  fs::path path;
};

struct LocalIncludeNode final : AstNode {
  LocalIncludeNode(std::string const &str) noexcept : path(str) {}
  ~LocalIncludeNode() final = default;
  auto accept(AstVisitor &) -> void final;

  fs::path path;
};

// TODO: there is a bug where an extraneous MACRO tkn is being pushed back,
// causing this #define node to be treated like it has a value, as opposed to
// just being a #define MACRO
struct DefineNode final : AstNode {
  DefineNode(std::string const &str, std::string &&lexeme) noexcept
      : name(str), lexeme(lexeme) {}
  DefineNode(std::string const &str) noexcept
      : name(str), lexeme(std::nullopt) {}

  ~DefineNode() final = default;
  auto accept(AstVisitor &) -> void final;

  std::string name;
  std::optional<std::string> lexeme;
};

struct DefineFuncNode final : AstNode {
  DefineFuncNode(std::string &&name, std::vector<std::string> &&parameters,
                 std::string &&body) noexcept
      : name(name), parameters(parameters), body(body) {}
  ~DefineFuncNode() final = default;
  auto accept(AstVisitor &) -> void final;

  std::string name;
  std::vector<std::string> parameters;
  std::string body;
};

struct UndefNode final : AstNode {
  UndefNode(std::string const &str) noexcept : name(str) {}
  ~UndefNode() final = default;
  auto accept(AstVisitor &visitor) -> void final;

  std::string name;
};

struct PragmaNode final : AstNode {
  PragmaNode(std::string const &value) noexcept : value(value) {}
  ~PragmaNode() final = default;
  auto accept(AstVisitor &visitor) -> void final;

  std::string value;
};

struct Ast final {
  /// @throws std::bad_alloc
  Ast();
  Ast(Ast const &) = delete;
  Ast &operator=(Ast const &) = delete;
  Ast(Ast &&) noexcept = default;
  Ast &operator=(Ast &&) noexcept = default;
  ~Ast() noexcept = default;

  std::vector<std::unique_ptr<AstNode>> nodes;

  auto accept(AstVisitor &) const -> void;
};

// TODO: rewrite this namespace, it really doesn't need to be written like this
namespace Expressions {
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

auto constexpr to_string(expr_t t) noexcept -> std::string_view {
  switch (t) {
  case expr_t::LPAREN:
    return std::string_view{"LPAREN"};
  case expr_t::RPAREN:
    return std::string_view{"RPAREN"};
  case expr_t::AND:
    return std::string_view{"AND"};
  case expr_t::OR:
    return std::string_view{"OR"};
  case expr_t::BIT_AND:
    return std::string_view{"BIT_AND"};
  case expr_t::BIT_OR:
    return std::string_view{"BIT_OR"};
  case expr_t::DEFINED:
    return std::string_view{"DEFINED"};
  case expr_t::LESS:
    return std::string_view{"LESS"};
  case expr_t::LESS_EQ:
    return std::string_view{"LESS_EQ"};
  case expr_t::GREATER:
    return std::string_view{"GREATER"};
  case expr_t::GREATER_EQ:
    return std::string_view{"GREATER_EQ"};
  case expr_t::STRINGIZING:
    return std::string_view{"STRINGIZING"};
  case expr_t::CONCAT:
    return std::string_view{"CONCAT"};
  case expr_t::PLUS:
    return std::string_view{"PLUS"};
  case expr_t::MINUS:
    return std::string_view{"MINUS"};
  case expr_t::STAR:
    return std::string_view{"STAR"};
  case expr_t::SLASH:
    return std::string_view{"SLASH"};
  case expr_t::BANG_EQ:
    return std::string_view{"BANG_EQ"};
  case expr_t::EQ:
    return std::string_view{"EQ"};
  case expr_t::EQ_EQ:
    return std::string_view{"EQ_EQ"};
  case expr_t::BANG:
    return std::string_view{"BANG"};
  case expr_t::LIT_CHAR:
    return std::string_view{"LIT_CHAR"};
  case expr_t::LIT_DEC:
    return std::string_view{"LIT_DEC"};
  case expr_t::LIT_HEX:
    return std::string_view{"LIT_HEX"};
  case expr_t::LIT_OCT:
    return std::string_view{"LIT_OCT"};
  case expr_t::LIT_BIN:
    return std::string_view{"LIT_BIN"};
  case expr_t::LIT_FLOAT:
    return std::string_view{"LIT_FLOAT"};
  case expr_t::MACRO:
    return std::string_view{"MACRO"};
  }
  unreachable();
}

struct ExprLexer final {
  std::vector<expr_t> tkns;
  std::vector<std::string> macros;
  auto to_ast(allocator::Page &) const -> ExprNode;

  auto constexpr matching(expr_t tkn, std::initializer_list<expr_t> &&matches)
      const noexcept -> bool {
    for (auto &&t : matches) {
      if (tkn == t) {
        return true;
      }
    }
    return false;
  }

  auto expression(allocator::Page &, size_t &, size_t &) const -> ExprNode;
  auto _or(allocator::Page &, size_t &, size_t &) const -> ExprNode;
  auto _and(allocator::Page &, size_t &, size_t &) const -> ExprNode;
  auto equality(allocator::Page &, size_t &, size_t &) const -> ExprNode;
  auto comparison(allocator::Page &, size_t &, size_t &) const -> ExprNode;
  auto term(allocator::Page &, size_t &, size_t &) const -> ExprNode;
  auto factor(allocator::Page &, size_t &, size_t &) const -> ExprNode;
  auto unary(allocator::Page &, size_t &, size_t &) const -> ExprNode;
  auto primary(allocator::Page &, size_t &, size_t &) const -> ExprNode;

  auto expect(size_t cur_t, expr_t &&tkn) const -> void {
    if (tkns[cur_t] != tkn) {
      throw std::runtime_error(
          std::format("Unexpected token, expected {}, found {}", to_string(tkn),
                      to_string(tkns[cur_t])));
    }
  }
#ifdef DEBUG_CPP
  auto display(std::ostream &) const noexcept -> std::ostream &;
#endif // DEBUG_CPP
};

auto lex(std::string_view const) -> ExprLexer;
auto lex_integer(std::string_view const, size_t &, std::vector<expr_t> &,
                 std::vector<std::string> &) -> void;
auto expand(ExprLexer const &, pp::MacroMap const &, StringSet const &)
    -> ExprLexer;
auto expand_macro(std::string const &, pp::MacroMap const &,
                  StringSet const &) noexcept -> ExprLexer;

auto eval_impl(ExprNode const &, pp::MacroMap const &, StringSet const &)
    -> int;

auto make_binary(allocator::Page &, expr_t, ExprNode &&, ExprNode &&) noexcept
    -> ExprNode;
auto make_unary(allocator::Page &, expr_t, ExprNode &&) noexcept -> ExprNode;
auto make_integer(expr_t, std::string_view) noexcept -> ExprNode;
}; // namespace Expressions

auto constexpr ExprNode::to_string(Expr_t t) noexcept -> std::string_view {
  switch (t) {
  case Expr_t::INT:
    return std::string_view{"INT"};
  case Expr_t::NUMBER:
    return std::string_view{"NUMBER"};
  case Expr_t::DEFINED:
    return std::string_view{"DEFINED"};
  case Expr_t::CHARLIT:
    return std::string_view{"CHARLIT"};
  case Expr_t::GROUPING:
    return std::string_view{"GROUPING"};
  case Expr_t::BINARY:
    return std::string_view{"BINARY"};
  case Expr_t::UNARY:
    return std::string_view{"UNARY"};
  case Expr_t::NONE:
    return std::string_view{"NONE"};
  }
  unreachable();
}

// NOTE: no need for a virtual dtor bc you shouldn't be dynamically allocating
// this ABC
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

#ifdef DEBUG_CPP
struct AstPrinter final : AstVisitor {
  std::ostream &out;
  // i really love that in c++ this is a thing you can do :)
  std::allocator_traits<std::string::allocator_type>::size_type depth;
  AstPrinter(std::ostream &out) noexcept : out(out), depth(0) {}

  auto print(Ast &ast) -> void {
    out << "AstPrinter:" NL;
    for (auto &&node : ast.nodes) {
      node->accept(*this);
    }
    out << "---" NL;
    out.flush();
  }

  constexpr auto get_indents() -> std::string {
    return std::string(depth, ' ');
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
#endif // DEBUG_CPP

// TODO: rewrite this implimentation so that the vector of paths is just
// returned instead of being a part of this struct
// TODO: expose this so that it can be initialized outside of this file, and we
// can then just hold onto it, allowing us to avoid reinitializing the arena for
// every file
struct AstIncluder final : AstVisitor {
  std::vector<fs::path> &paths;
  pp::MacroMap &macros;
  pp::StringSet &defs;

  allocator::Page &alloc;

  AstIncluder(std::vector<fs::path> &paths, allocator::Page &alloc,
              pp::MacroMap &macros, pp::StringSet &defs) noexcept
      : paths(paths), macros(macros), defs(defs), alloc(alloc) {}

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
  case ir_t::VARIADIC:
    return std::string_view("VARIADIC");
  }
  unreachable();
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
  static auto const keywords = std::unordered_map<std::string_view, ir_t>{{
    {std::string_view{"#if"}, ir_t::IF},
    {std::string_view{"#ifdef"}, ir_t::IFDEF},
    {std::string_view{"#ifndef"}, ir_t::IFNDEF},
    {std::string_view{"#elif"}, ir_t::ELIF},
    {std::string_view{"#else"}, ir_t::ELSE},
    {std::string_view{"#endif"}, ir_t::ENDIF},
    {std::string_view{"#define"}, ir_t::DEFINE},
    {std::string_view{"#include"}, ir_t::INCLUDE},
    {std::string_view{"#undef"}, ir_t::UNDEF},
    {std::string_view{"#pragma"}, ir_t::PRAGMA}
  }};
  auto constexpr chars_of_interest = std::string_view{"#/\"'"};
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

      auto const hash_keyword = std::string_view{start + i, start + end};
      i = end;
      auto const keyword = keywords.find(hash_keyword);
      if (keyword == keywords.end()) {
        // continue to next character of interest
        i = luamake::skip_until(chars_of_interest, fcontent, i);
        throw std::runtime_error(
            std::format("Hash keyword [{}], is not implimented", hash_keyword));
      }

      switch (keyword->second) {
      case ir_t::INCLUDE: {
        if (i >= file.size())
          throw std::runtime_error("Unable to parse include parameter");
        i = skip_ws(fcontent, i);
        if (i >= file.size())
          throw std::runtime_error("Unable to parse include parameter");
        lex.types.push_back(ir_t::INCLUDE);
        switch (fcontent[i]) {
        case '<': {
          lex.types.push_back(ir_t::LANGLE);
          end = luamake::skip_until('>', fcontent, i + 1);
          if (end >= file.size())
            throw std::runtime_error("Non terminated global include");
          lex.types.push_back(ir_t::LIT_STRING);
          lex.lexemes.push_back(std::string(start + i + 1, start + end));
          i = end + 1;
          lex.types.push_back(ir_t::RANGLE);
        } break;
        case '"': {
          lex.types.push_back(ir_t::QUOTE);
          end = luamake::skip_until('"', fcontent, i + 1);
          if (end >= file.size())
            throw std::runtime_error("Non terminated local include");
          lex.types.push_back(ir_t::LIT_STRING);
          lex.lexemes.push_back(std::string(start + i + 1, start + end));
          i = end + 1;
          lex.types.push_back(ir_t::QUOTE);
        } break;
        default:
          throw std::runtime_error(
              std::format("character found = {}", fcontent[i]));
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
        lex.push_lexeme(std::string(start + i, start + end));
        i = end;
        switch (fcontent[i]) {
        case '(': {
          lex.types.push_back(ir_t::LPAREN);
          // we don't need to skip any ws, because otherwise the macro wouldn't
          // be a function like macro
          i = lex.parse_define_args(fcontent, skip_ws(fcontent, i + 1));
          if (fcontent[i] != ')') {
            throw std::runtime_error("Expected closing ')' when parsing "
                                     "function macro arguments");
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
        i = lex.produce_lexeme(fcontent, skip_ws(fcontent, i));
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
#ifdef DEBUG_CPP
        std::cerr << WARNING "Unknown ir_t preprocessor directive ["
                  << to_string(keyword->second) << "]" NORMAL NL;
#endif // DEBUG_CPP
        lex.types.push_back(keyword->second);
        break;
      }
    } break;
    case '/': {
      if (!(i + 1 < file.size())) {
        throw std::runtime_error("'/' found at end of file");
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
          throw std::runtime_error("Non terminated multi line comment");
      } break;
      default: // probably just an op /
        i = luamake::skip_until(chars_of_interest, fcontent, i + 1);
        break;
      }
    } break;
    case '"':
      [[fallthrough]]; // both cases are handled the same
    case '\'': {
      auto const ch = fcontent[i];
      do {
        // +1 b/c fcontent[i] == '"' | '\'', and if not then we'll be out of
        // bounds so it doesn't matter
        i = luamake::skip_until(ch, fcontent, i + 1);
        if (!(i < file.size())) {
          throw std::runtime_error("Non terminated char");
        }
        // the case when you have '\\'
      } while (fcontent[i - 1] == '\\' && fcontent[i - 2] != '\\');
      ++i;
    } break;
    default:
      i = luamake::skip_until(chars_of_interest, fcontent, i + 1);
      break;
    }
  }
#ifdef DEBUG_CPP
  std::cout << "Lexer:" NL;
  lex.display(std::cout).flush();
#endif // DEBUG_CPP
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

auto Lexer::produce_macro(std::string_view const buf, size_t i) -> size_t {
  auto constexpr ws = std::string_view{" \t\r\n"};
  auto constexpr switch_chars = std::string_view{"\\\n/"};
  auto macro = std::string();
  auto start = i;
  auto looping = true;
  while (i < buf.size() && looping) {
    auto const ch = buf[i];
    switch (ch) {
    case '\\': {
      if (i - 1 > start) {
        auto const mac =
            std::string_view{buf.begin() + start, buf.begin() + i - 1};
        macro.append(mac);
      }
      ++i;
      if (i < buf.size() && buf[i] == '\n')
        ++i;
      start = i = luamake::skip_while(ws, buf, i);
    } break;
    case '/': {
      if (i + 1 < buf.size() && buf[i + 1] == '/') {
        auto end = i - 1;
        while (is_any_of(ws, buf[end])) {
          --end;
        }
        if (end + 1 > start) {
          auto const mac =
              std::string_view{buf.begin() + start, buf.begin() + end + 1};
          macro.append(mac);
        }
        looping = false;
      } else {
        i = luamake::skip_until(switch_chars, buf, i + 1);
      }
    } break;
    case '\n': {
      auto const mac = std::string_view{buf.begin() + start, buf.begin() + i};
      macro.append(mac);
      looping = false;
    } break;
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

auto Lexer::produce_lexeme(std::string_view const buf, size_t i) -> size_t {
  auto constexpr ws = std::string_view{" \t\r\n"};
  auto constexpr switch_chars = std::string_view{"\\\n/"};
  auto lexeme = std::string();
  auto start = i;
  auto looping = true;
  while (i < buf.size() && looping) {
    auto const ch = buf[i];
    switch (ch) {
    case '\\': {
      if (i - 1 > start) {
        auto const lex =
            std::string_view{buf.begin() + start, buf.begin() + i - 1};
        lexeme.append(lex);
      }
      ++i;
      if (i < buf.size() && buf[i] == '\n')
        ++i;
      start = i = luamake::skip_while(ws, buf, i);
    } break;
    case '/': {
      if (i + 1 < buf.size() && buf[i + 1] == '/') {
        auto end = i - 1;
        while (is_any_of(ws, buf[end])) {
          --end;
        }
        auto const lex =
            std::string_view{buf.begin() + start, buf.begin() + end + 1};
        lexeme.append(lex);
        looping = false;
      } else {
        i = luamake::skip_until(switch_chars, buf, i + 1);
      }
    } break;
    case '\n': {
      auto const lex = std::string_view{buf.begin() + start, buf.begin() + i};
      lexeme.append(lex);
      looping = false;
    } break;
    default:
      i = luamake::skip_until(switch_chars, buf, i);
      break;
    }
  }
  push_lexeme(lexeme);
  // the only way to break out of the loop is to hit a '\n' char, but we don't
  // want to include that in the string, we do want to skip over it though so we
  // add 1 here
  return i + 1;
}

auto Lexer::parse_define_args(std::string_view const fcontent, size_t i)
    -> size_t {
  auto constexpr switch_chars = std::string_view{"),. \t\r\n"};
  while (i < fcontent.size()) {
    switch (fcontent[i]) {
    case ')':
      return i;
    case '.': {
      if (i + 3 < fcontent.size() &&
          (fcontent[i + 1] == '.' && fcontent[i + 2] == '.')) {
        types.push_back(ir_t::VARIADIC);
        i += 3;
      } else {
        throw std::runtime_error(
            "Error around `.` in function macro parameters");
      }
    } break;
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
  throw std::runtime_error("Unterminated function macro arguments");
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
    throw std::runtime_error(
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
  auto then_branch = std::vector<ptr<AstNode>>();
  auto elif_branches = std::vector<ptr<ElifNode>>();
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
      throw std::runtime_error(
          std::format("Unexpected token [{}] found in top level scope.",
                      to_string(types[cur_t])));
    }
  }

  if (cur == FoundEnd::none) {
    throw std::runtime_error(
        std::format("Unterminated #ifdef directive found"));
  }

  auto const determine_state = [this](auto const cur_t) {
    switch (types[cur_t]) {
    case ir_t::ELSE:
      return FoundEnd::_else;
    case ir_t::ENDIF:
      return FoundEnd::endif;
    case ir_t::ELIF:
      return FoundEnd::elif;
    default:
      throw std::runtime_error(std::format(
          "Unexpected token [{}], found after parsing #else directive",
          to_string(types[cur_t])));
    }
  };
  while (cur != FoundEnd::none) {
    switch (cur) {
    case FoundEnd::elif:
      if (else_branch != nullptr) {
        throw std::runtime_error(
            std::format("Found #elif directive following #else "
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
        throw std::runtime_error(
            "Found multiple #else directives attached to a single "
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
    throw std::runtime_error(std::format("Expected lexeme following #ifdef"));
  }
  ++cur_t;
  auto lex = lexemes[cur_lex++];
  auto then_branch = std::vector<ptr<AstNode>>();
  auto elif_branches = std::vector<ptr<ElifNode>>();
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
      throw std::runtime_error(
          std::format("Unexpected token [{}] found in top level scope.",
                      to_string(types[cur_t])));
    }
  }

  if (cur == FoundEnd::none) {
    throw std::runtime_error(
        std::format("Unterminated #ifdef directive found"));
  }

  auto const determine_state = [this](auto const cur_t) {
    switch (types[cur_t]) {
    case ir_t::ELSE:
      return FoundEnd::_else;
    case ir_t::ENDIF:
      return FoundEnd::endif;
    case ir_t::ELIF:
      return FoundEnd::elif;
    default:
      throw std::runtime_error(std::format(
          "Unexpected token [{}], found after parsing #else directive",
          to_string(types[cur_t])));
    }
  };
  while (cur != FoundEnd::none) {
    switch (cur) {
    case FoundEnd::elif:
      if (else_branch != nullptr) {
        throw std::runtime_error(
            std::format("Found #elif directive following #else "
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
        throw std::runtime_error(
            "Found multiple #else directives attached to a single "
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
    throw std::runtime_error(std::format("Expected lexeme following #ifndef"));
  }
  ++cur_t;
  auto lex = lexemes[cur_lex++];
  auto then_branch = std::vector<ptr<AstNode>>();
  auto elif_branches = std::vector<ptr<ElifNode>>();
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
      throw std::runtime_error(
          std::format("Unexpected token [{}] found in top level scope.",
                      to_string(types[cur_t])));
    }
  }

  if (cur == FoundEnd::none) {
    throw std::runtime_error(
        std::format("Unterminated #ifndef directive found"));
  }

  auto const determine_state = [this](auto const cur_t) {
    switch (types[cur_t]) {
    case ir_t::ELSE:
      return FoundEnd::_else;
    case ir_t::ENDIF:
      return FoundEnd::endif;
    case ir_t::ELIF:
      return FoundEnd::elif;
    default:
      throw std::runtime_error(std::format(
          "Unexpected token [{}], found after parsing #else directive",
          to_string(types[cur_t])));
    }
  };
  while (cur != FoundEnd::none) {
    switch (cur) {
    case FoundEnd::elif:
      if (else_branch != nullptr) {
        throw std::runtime_error(
            std::format("Found #elif directive following #else "
                        "directive in #ifndef directive"));
      }
      while (cur_t < types.size() &&
             (types[cur_t] != ir_t::ELSE || types[cur_t] != ir_t::ENDIF)) {
        elif_branches.push_back(handle_elif(cur_t, cur_lex));
      }
      cur = determine_state(cur_t);
      break;
    case FoundEnd::_else:
      if (else_branch != nullptr) {
        throw std::runtime_error(
            "Found multiple #else directives attached to a single "
            "#ifndef directive");
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
    auto parameters = std::vector<std::string>();
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
    throw std::runtime_error(
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
    throw std::runtime_error(std::format("Malformed #include statement, "
                                         "expected '<' or '\"', found [{}]",
                                         to_string(types[cur_t])));
  }
  }
}

auto Lexer::handle_pragma(size_t &cur_t, size_t &cur_lex)
    -> std::unique_ptr<AstNode> {
  ++cur_t;
  if (types[cur_t] != ir_t::LEXEME)
    throw std::runtime_error(
        std::format("Expected lexeme in #pragma preprocessor directive"));
  ++cur_t;
  auto value = lexemes[cur_lex++];
  return std::make_unique<PragmaNode>(value);
}

auto Lexer::handle_elif(size_t &cur_t, size_t &cur_lex) -> ptr<ElifNode> {
  ++cur_t;
  expect(cur_t, ir_t::MACRO);
  auto condition = lexemes[cur_lex++];
  ++cur_t;

  auto then_branch = std::vector<ptr<AstNode>>();
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
      throw std::runtime_error(
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
  auto res = std::vector<ptr<AstNode>>();
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
      throw std::runtime_error(
          std::format("Unexpected token [{}] found in top level scope.",
                      to_string(types[cur_t])));
    }
  }
  return std::make_unique<ElseNode>(std::move(res));
}

auto Lexer::expect(size_t cur_t, ir_t tkn, std::string_view calling_func)
    -> void {
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
      throw std::runtime_error(std::format(
          "Attempting to index out of bounds of Lexer::types "
          "array, looking for token [{}], from calling function = [{}]",
          to_string(tkn), calling_func));
    } else {
      throw std::runtime_error(
          std::format("Attempting to index out of bounds of Lexer::types "
                      "array, looking for token [{}]",
                      to_string(tkn)));
    }
    return;
  case FailReason::Unexpected:
    if (calling_func != "") {
      throw std::runtime_error(
          std::format("Unexpected token, expected {}, found {}, "
                      "from calling function = [{}]",
                      to_string(tkn), to_string(types[cur_t]), calling_func));
    } else {
      throw std::runtime_error(
          std::format("Unexpected token, expected {}, found {}", to_string(tkn),
                      to_string(types[cur_t])));
    }
    return;
  }
}

#ifdef DEBUG_CPP
auto Lexer::display(std::ostream &out) const noexcept -> std::ostream & {
  out << "Types:" NL "\t";
  for (auto const &type : types) {
    out << '[' << to_string(type) << ']';
  }
  out << NL;
  out << "Lexemes:" NL "\t";
  for (auto const &lexeme : lexemes) {
    out << '[' << lexeme << ']';
  }
  out << NL;
  return out;
}
#endif // DEBUG_CPP

ExprNode::ExprNode() noexcept {
  // TODO: we should be able to remove this step, assuming we've done everything
  // correct, we'll leave it in debug mode ig(?)
  std::memset(storage.data(), 0, STORAGE_SIZE);
  new (storage.data()) Expr_t(Expr_t::NONE);
}

ExprNode::~ExprNode() noexcept {
  // have to manually call the dtor because we placement new them
  switch (expr_t()) {
  case Expr_t::DEFINED:
    [[fallthrough]];
  case Expr_t::CHARLIT: {
    if (storage[sizeof(Expr_t)] == 0xbe) {
      // small string, all on the stack, nothing to do
    } else if (storage[sizeof(Expr_t)] == 0xff) {
      auto *buffer = static_cast<char *>(nullptr);
      std::memcpy(&buffer, storage.data() + 2 * sizeof(size_t), sizeof(char *));
      // we call this with new[] in the ctor, it's just nested in the placement
      // new call
      delete[] buffer;
    } else {
      std::cerr << "Memory corruption with the storage buffer";
      std::terminate();
    }
  } break;
  case Expr_t::GROUPING: {
    auto *grp = reinterpret_cast<ExprNode *>(storage.data() + sizeof(size_t));
    grp->~ExprNode();
  } break;
  case Expr_t::BINARY: {
    auto *lhs = reinterpret_cast<ExprNode *>(storage.data() + sizeof(size_t));
    auto *rhs =
        reinterpret_cast<ExprNode *>(storage.data() + 2 * sizeof(size_t));
    lhs->~ExprNode();
    rhs->~ExprNode();
  } break;
  case Expr_t::UNARY: {
    auto *un = reinterpret_cast<ExprNode *>(storage.data() + sizeof(size_t));
    un->~ExprNode();
  } break;
  case Expr_t::INT:
    [[fallthrough]];
  case Expr_t::NUMBER:
    [[fallthrough]];
  case Expr_t::NONE:
    break; // nothing to do, all on the stack
  }
}

ExprNode::ExprNode(ExprNode &&that) noexcept
    // std::memcpy(storage.data(), that.storage.data(), STORAGE_SIZE); ?
    : storage(std::move(that.storage)) {
  // tag that so that when it's dtor is called nothing happens
  new (that.storage.data()) Expr_t(Expr_t::NONE);
}
auto ExprNode::operator=(ExprNode &&that) noexcept -> ExprNode & {
  // both *this and that should be correctly constructed objects so this
  // *should* work, assuming that they're actually swapping the array's and not
  // the pointers
  std::swap(storage, that.storage);
  return *this;
}

auto ExprNode::expr_t() const noexcept -> ExprNode::Expr_t {
  auto expr_t = Expr_t{};
  std::memcpy(&expr_t, storage.data(), sizeof(Expr_t));
  return expr_t;
}

auto ExprNode::eval(std::string_view const expr, allocator::Page &page,
                    pp::MacroMap const &macros, StringSet const &def_macros)
    -> int {
  // TODO: idk fix these, they should be just one call(?)
  auto const expr_lex = Expressions::lex(expr);
  auto const expansion = Expressions::expand(expr_lex, macros, def_macros);
  // TODO: report if the expansion is empty, i.e. if you have a case like
  // ```
  // #define MACRO
  // #if MACRO
  // #endif
  // ```
  // in that case, this becomes a malformed program
  auto const ast = expansion.to_ast(page);
#ifdef DEBUG_CPP
  ast.display(std::cout) << std::endl;
#endif // DEBUG_CPP
  return Expressions::eval_impl(ast, macros, def_macros);
}

#ifdef DEBUG_CPP
auto ExprNode::to_string() const noexcept -> std::string {
  auto str = std::string();
  str += '[';
  str += to_string(expr_t());
  str += ']';
  switch (expr_t()) {
  case Expr_t::INT: {
    str += "{.i=";
    auto val = size_t{};
    std::memcpy(&val, storage.data() + sizeof(size_t), sizeof(size_t));
    str += std::to_string(val);
    str += "}";
  } break;
  case Expr_t::NUMBER: {
    str += "{.f=";
    auto val = double{};
    std::memcpy(&val, storage.data() + sizeof(size_t), sizeof(double));
    str += std::to_string(val);
    str += "}";
  } break;
  case Expr_t::DEFINED: {
    str += "{.def=";
    str += to<ExprNode::Defined>().str;
    str += "}";
  } break;
  case Expr_t::CHARLIT: {
    str += "{.char_lit=";
    str += to<ExprNode::CharLit>().str;
    str += "}";
  } break;
  case Expr_t::GROUPING: {
    str += "{.grp=";
    str += to<ExprNode::Grouping>().expr->to_string();
    str += "}";
  } break;
  case Expr_t::BINARY: {
    str += "{.bin_t=";
    auto const bin_t = meta_data<ExprNode::Binary::Binary_t>();
    switch (bin_t) {
    case ExprNode::Binary::Binary_t::PLUS:
      str += '+';
      break;
    case ExprNode::Binary::Binary_t::MINUS:
      str += '-';
      break;
    case ExprNode::Binary::Binary_t::TIMES:
      str += '*';
      break;
    case ExprNode::Binary::Binary_t::DIVIDE:
      str += '/';
      break;
    case ExprNode::Binary::Binary_t::GREATER:
      str += '>';
      break;
    case ExprNode::Binary::Binary_t::GREATER_EQ:
      str += ">=";
      break;
    case ExprNode::Binary::Binary_t::LESS:
      str += '<';
      break;
    case ExprNode::Binary::Binary_t::LESS_EQ:
      str += "<=";
      break;
    case ExprNode::Binary::Binary_t::NEQ:
      str += "!=";
      break;
    case ExprNode::Binary::Binary_t::EQ:
      str += "==";
      break;
    case ExprNode::Binary::Binary_t::AND:
      str += "&&";
      break;
    case ExprNode::Binary::Binary_t::OR:
      str += "||";
      break;
    }
    str += ",lhs=";
    str += to<ExprNode::Binary>().lhs->to_string();
    str += ",rhs=";
    str += to<ExprNode::Binary>().rhs->to_string();
    str += "}";
  } break;
  case Expr_t::UNARY: {
    str += "{.un_t=";
    auto const un_t = meta_data<ExprNode::Unary::Unary_t>();
    switch (un_t) {
    case Unary::Unary_t::BANG:
      str += '!';
      break;
    case Unary::Unary_t::MINUS:
      str += '-';
      break;
    }
    str += ",un=";
    str += to<ExprNode::Unary>().un->to_string();
    str += "}";
  } break;
  case Expr_t::NONE: {
    str += "{}";
  } break;
  }
  return str;
}

auto ExprNode::display(std::ostream &out) const noexcept -> std::ostream & {
  return out << to_string();
}
#endif // DEBUG_CPP

Ast::Ast() { nodes.reserve(20); }

auto Expressions::ExprLexer::to_ast(allocator::Page &page) const -> ExprNode {
  auto cur_t = size_t{};
  auto cur_lex = size_t{};
  return expression(page, cur_t, cur_lex);
}

auto Expressions::ExprLexer::expression(allocator::Page &page, size_t &cur_t,
                                        size_t &cur_lex) const -> ExprNode {
  return _or(page, cur_t, cur_lex);
}

auto Expressions::ExprLexer::_or(allocator::Page &page, size_t &cur_t,
                                 size_t &cur_lex) const -> ExprNode {
  auto lhs = _and(page, cur_t, cur_lex);
  while (cur_t < tkns.size() && matching(tkns[cur_t], {expr_t::OR})) {
    auto const tkn = tkns[cur_t++];
    auto rhs = _and(page, cur_t, cur_lex);
    lhs = make_binary(page, tkn, std::move(lhs), std::move(rhs));
  }
  return lhs;
}

auto Expressions::ExprLexer::_and(allocator::Page &page, size_t &cur_t,
                                  size_t &cur_lex) const -> ExprNode {
  auto lhs = equality(page, cur_t, cur_lex);
  while (cur_t < tkns.size() && matching(tkns[cur_t], {expr_t::AND})) {
    auto const tkn = tkns[cur_t++];
    auto rhs = equality(page, cur_t, cur_lex);
    lhs = make_binary(page, tkn, std::move(lhs), std::move(rhs));
  }
  return lhs;
}

auto Expressions::ExprLexer::equality(allocator::Page &page, size_t &cur_t,
                                      size_t &cur_lex) const -> ExprNode {
  auto lhs = comparison(page, cur_t, cur_lex);
  while (cur_t < tkns.size() &&
         matching(tkns[cur_t], {expr_t::BANG_EQ, expr_t::EQ_EQ})) {
    auto const tkn = tkns[cur_t++];
    auto rhs = comparison(page, cur_t, cur_lex);
    lhs = make_binary(page, tkn, std::move(lhs), std::move(rhs));
  }
  return lhs;
}

auto Expressions::ExprLexer::comparison(allocator::Page &page, size_t &cur_t,
                                        size_t &cur_lex) const -> ExprNode {
  auto lhs = term(page, cur_t, cur_lex);
  while (cur_t < tkns.size() &&
         matching(tkns[cur_t], {expr_t::LESS, expr_t::LESS_EQ, expr_t::GREATER,
                                expr_t::GREATER_EQ})) {
    auto const tkn = tkns[cur_t++];
    auto rhs = term(page, cur_t, cur_lex);
    lhs = make_binary(page, tkn, std::move(lhs), std::move(rhs));
  }
  return lhs;
}

auto Expressions::ExprLexer::term(allocator::Page &page, size_t &cur_t,
                                  size_t &cur_lex) const -> ExprNode {
  auto lhs = factor(page, cur_t, cur_lex);
  while (cur_t < tkns.size() &&
         matching(tkns[cur_t], {expr_t::PLUS, expr_t::MINUS})) {
    auto const tkn = tkns[cur_t++];
    auto rhs = factor(page, cur_t, cur_lex);
    lhs = make_binary(page, tkn, std::move(lhs), std::move(rhs));
  }
  return lhs;
}

auto Expressions::ExprLexer::factor(allocator::Page &page, size_t &cur_t,
                                    size_t &cur_lex) const -> ExprNode {
  auto lhs = unary(page, cur_t, cur_lex);
  while (cur_t < tkns.size() &&
         matching(tkns[cur_t], {expr_t::STAR, expr_t::SLASH})) {
    auto const tkn = tkns[cur_t++];
    auto rhs = unary(page, cur_t, cur_lex);
    lhs = make_binary(page, tkn, std::move(lhs), std::move(rhs));
  }
  return lhs;
}

auto Expressions::ExprLexer::unary(allocator::Page &page, size_t &cur_t,
                                   size_t &cur_lex) const -> ExprNode {
  if (cur_t < tkns.size() &&
      matching(tkns[cur_t], {expr_t::BANG, expr_t::MINUS})) {
    auto const tkn = tkns[cur_t++];
    auto un = unary(page, cur_t, cur_lex);
    return make_unary(page, tkn, std::move(un));
  }
  return primary(page, cur_t, cur_lex);
}

auto Expressions::ExprLexer::primary(allocator::Page &page, size_t &cur_t,
                                     size_t &cur_lex) const -> ExprNode {
  // TODO
  switch (tkns[cur_t]) {
  case expr_t::MACRO:
    break;
  case expr_t::LIT_CHAR:
    break;
  case expr_t::LIT_DEC:
    ++cur_t;
    return make_integer(expr_t::LIT_DEC, macros[cur_lex++]);
  case expr_t::LIT_HEX:
    ++cur_t;
    return make_integer(expr_t::LIT_HEX, macros[cur_lex++]);
    break;
  case expr_t::LIT_OCT:
    break;
  case expr_t::LIT_BIN:
    break;
  case expr_t::LIT_FLOAT:
    break;
  case expr_t::LPAREN: {
    ++cur_t;
    auto res = expression(page, cur_t, cur_lex);
    if (tkns[cur_t] != expr_t::RPAREN) {
      throw std::runtime_error(
          std::format("While parsing a grouping expression, "
                      "expected a ')' to wrap the expression"));
    }
    ++cur_t;
    auto *expr_ptr = static_cast<ExprNode *>(page.alloc(sizeof(ExprNode)));
    *expr_ptr = std::move(res);
    return ExprNode::from(ExprNode::Grouping{expr_ptr});
  } break;
  case expr_t::DEFINED: {
    ++cur_t;
    auto lex = std::string();
    if (tkns[cur_t] == expr_t::LPAREN) {
      ++cur_t;
      expect(cur_t, expr_t::MACRO);
      ++cur_t;
      lex = macros[cur_lex++];
      expect(cur_t, expr_t::RPAREN);
      ++cur_t;
    } else {
      expect(cur_t, expr_t::MACRO);
      ++cur_t;
      lex = macros[cur_lex++];
    }
    return ExprNode::from(ExprNode::Defined{std::move(lex)});
  } break;

  default:
    throw std::runtime_error(
        std::format("Unexpected token [{}] found while parsing an expression",
                    to_string(tkns[cur_t])));
  }
  throw std::runtime_error(std::format(
      "In function {}, support for token [{}] is not currently implimented",
      __FUNCTION__, to_string(tkns[cur_t])));
}

#ifdef DEBUG_CPP
auto Expressions::ExprLexer::display(std::ostream &out) const noexcept
    -> std::ostream & {
  out << "Tokens:" NL "\t";
  for (auto const &type : tkns) {
    out << '[' << to_string(type) << ']';
  }
  out << NL;
  out << "Macros:" NL "\t";
  for (auto const &macro : macros) {
    out << '[' << macro << ']';
  }
  out << NL;
  return out;
}
#endif // DEBUG_CPP

auto Expressions::lex(std::string_view const str) -> ExprLexer {
  auto constexpr defined_str = std::string_view{"defined"};
  auto tkns = std::vector<expr_t>();
  auto macros = std::vector<std::string>();
  for (auto i = size_t{}; i < str.size();) {
    // TODO: probably add a macro for these basic types so that we don't have to
    // write out a bunch of things every time, and so that this function can be
    // smaller
    switch (auto ch = str[i]) {
    case '+':
      tkns.push_back(expr_t::PLUS);
      ++i;
      break;
    case '(':
      tkns.push_back(expr_t::LPAREN);
      ++i;
      break;
    case ')':
      tkns.push_back(expr_t::RPAREN);
      ++i;
      break;
    case '|': {
      ++i;
      if (i < str.size() && str[i] == '|') {
        ++i;
        tkns.push_back(expr_t::OR);
      } else {
        tkns.push_back(expr_t::BIT_OR);
      }
    } break;
    case '&': {
      ++i;
      if (i < str.size() && str[i] == '&') {
        ++i;
        tkns.push_back(expr_t::AND);
      } else {
        tkns.push_back(expr_t::BIT_AND);
      }
    } break;
    case '=': {
      ++i;
      if (i < str.size() && str[i] == '=') {
        ++i;
        tkns.push_back(expr_t::EQ_EQ);
      } else {
        tkns.push_back(expr_t::EQ);
      }
    } break;
    case '!': {
      ++i;
      if (i < str.size() && str[i] == '=') {
        ++i;
        tkns.push_back(expr_t::BANG_EQ);
      } else {
        tkns.push_back(expr_t::BANG);
      }
    } break;
    case '<': {
      ++i;
      if (i < str.size() && str[i] == '=') {
        ++i;
        tkns.push_back(expr_t::LESS_EQ);
      } else {
        tkns.push_back(expr_t::LESS);
      }
    } break;
    case '>': {
      ++i;
      if (i < str.size() && str[i] == '=') {
        ++i;
        tkns.push_back(expr_t::GREATER_EQ);
      } else {
        tkns.push_back(expr_t::GREATER);
      }
    } break;
    case '#': {
      ++i;
      if (i < str.size() && str[i] == '#') {
        ++i;
        tkns.push_back(expr_t::STRINGIZING);
      } else {
        tkns.push_back(expr_t::CONCAT);
      }
    } break;
    case '.':
      throw std::runtime_error(
          std::format("found '.' while parsing expression [{}] in "
                      "a #if or #elif condition.",
                      str));
    case 'd': {
      if (i + defined_str.size() < str.size() &&
          strncmp(str.data() + i, defined_str.data(), defined_str.size()) ==
              0) {
        i += defined_str.size();
        tkns.push_back(expr_t::DEFINED);
      } else {
        auto const start = i;
        i = luamake::skip_until(delims_at(delims::LEXEME), str, i);
        tkns.push_back(expr_t::MACRO);
        macros.push_back(std::string(str.data() + start, str.data() + i));
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
        tkns.push_back(expr_t::MACRO);
        macros.push_back(std::string(str.data() + start, str.data() + i));
      }
    }
    }
  }
  return ExprLexer(tkns, macros);
}

// for now we just throw out any integer suffix, we'll have to actually add
// support for that
auto Expressions::lex_integer(std::string_view const str, size_t &i,
                              std::vector<expr_t> &tkns,
                              std::vector<std::string> &macros) -> void {
  auto constexpr integer_suffix = std::string_view{"ulzULZ"};
  if (str[i] != '0') {
    auto const start = i;
    i = luamake::skip_while(delims_at(delims::ALLOWED_DECIMAL), str, i);
    tkns.push_back(expr_t::LIT_DEC);
    macros.push_back(std::string(str.data() + start, str.data() + i));
    // NOTE: we technically need to worry about the order of things, for
    // instance, we allow code that looks like zlu, which isn't an allowed
    // integer suffix, but i don't care about fixing that right now
    i = skip_while(integer_suffix, str, i);
    return;
  }
  // str[i] == 0
  ++i;
  if (!(i < str.size())) {
    tkns.push_back(expr_t::LIT_DEC);
    macros.push_back(std::string(1, '0'));
    return;
  }

  // TODO: check if we're on c++14>=, bc otherwise this is supposed to
  // be an error, the same goes with "'" character
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
    // NOTE: apparently this only works with clang compilers, idk if its a non
    // standard extension, but for now we'll keep it, and if it causes issues
    // later we can fix it
    case 'o':
      return int_type::OCTAL;
    default:
      if (!is_digit(ch)) {
        throw std::runtime_error(std::format(
            "Found char [{}], while attempting to parse an integer", ch));
      } else {
        return int_type::OCTAL;
      }
    }
  }();
  ++i;

  // spacing to work with the format strings
  auto constexpr to_string = [](int_type int_t) -> std::string_view {
    switch (int_t) {
    case int_type::DECIMAL:
      return std::string_view{" decimal"};
    case int_type::BINARY:
      return std::string_view{" binary"};
    case int_type::HEX:
      return std::string_view{" hexadecimal"};
    case int_type::OCTAL:
      return std::string_view{"n octal"};
    }
    unreachable();
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
    unreachable();
  };
  auto allow_quote = true;
  auto const start = i;
  while (i < str.size()) {
    if (str[i] == '\'') {
      if (!allow_quote) {
        throw std::runtime_error(
            std::format("When parsing a{} integer, found two ' characters "
                        "back to back, these are treated as identifiers for a "
                        "char literal, and thus a formatting error.",
                        to_string(int_t)));
      } else {
        allow_quote = false;
        ++i;
      }
    } else {
      if (!is_allowed_char(int_t, str[i])) {
        break;
      }
      /*
      if (!is_allowed_char(int_t, str[i])) {
        throw std::runtime_error(std::format("While parsing a{} integer, found "
                                    "[{}], a not supported character",
                                    to_string(int_t), str[i]));
      }
      */
      ++i;
      allow_quote = true;
    }
  }
  tkns.push_back([](int_type int_t) {
    switch (int_t) {
    case int_type::DECIMAL:
      return expr_t::LIT_DEC;
    case int_type::BINARY:
      return expr_t::LIT_BIN;
    case int_type::OCTAL:
      return expr_t::LIT_OCT;
    case int_type::HEX:
      return expr_t::LIT_HEX;
    }
    unreachable();
  }(int_t));
  macros.push_back(std::string(str.data() + start, str.data() + i));
  i = skip_while(integer_suffix, str, i);
}

auto Expressions::eval_impl(ExprNode const &e, pp::MacroMap const &macros,
                            StringSet const &def_macros) -> int {
  switch (e.expr_t()) {
  case ExprNode::Expr_t::INT:
    return static_cast<int>(e.to<ExprNode::Integer>().i);
  case ExprNode::Expr_t::DEFINED:
    return is_defined(e.to<ExprNode::Defined>().str, macros, def_macros) ? 1
                                                                         : 0;
    // TODO: report this kind of error earlier
  case ExprNode::Expr_t::NUMBER:
    [[fallthrough]];
  case ExprNode::Expr_t::CHARLIT:
    throw std::runtime_error(
        std::format("While evaluating if expression found a not integer."));
  case ExprNode::Expr_t::GROUPING: {
    auto const group = e.to<ExprNode::Grouping>();
    return eval_impl(*group.expr, macros, def_macros);
  }
  case ExprNode::Expr_t::BINARY: {
    auto const bin = e.to<ExprNode::Binary>();
    auto const lhs = eval_impl(*bin.lhs, macros, def_macros);
    auto const rhs = eval_impl(*bin.rhs, macros, def_macros);
    switch (e.meta_data<ExprNode::Binary::Binary_t>()) {
    case ExprNode::Binary::Binary_t::PLUS:
      return lhs + rhs;
    case ExprNode::Binary::Binary_t::MINUS:
      return lhs - rhs;
    case ExprNode::Binary::Binary_t::TIMES:
      return lhs * rhs;
    case ExprNode::Binary::Binary_t::DIVIDE:
      return lhs / rhs;
    case ExprNode::Binary::Binary_t::GREATER:
      return lhs > rhs ? 1 : 0;
    case ExprNode::Binary::Binary_t::GREATER_EQ:
      return lhs >= rhs ? 1 : 0;
    case ExprNode::Binary::Binary_t::LESS:
      return lhs < rhs ? 1 : 0;
    case ExprNode::Binary::Binary_t::LESS_EQ:
      return lhs <= rhs ? 1 : 0;
    case ExprNode::Binary::Binary_t::NEQ:
      return lhs != rhs ? 1 : 0;
    case ExprNode::Binary::Binary_t::EQ:
      return lhs == rhs ? 1 : 0;
    case ExprNode::Binary::Binary_t::AND:
      return lhs && rhs ? 1 : 0;
    case ExprNode::Binary::Binary_t::OR:
      return lhs || rhs ? 1 : 0;
    }
  }
  case ExprNode::Expr_t::UNARY: {
    auto const un = e.to<ExprNode::Unary>();
    auto const res = eval_impl(*un.un, macros, def_macros);
    switch (e.meta_data<ExprNode::Unary::Unary_t>()) {
    case ExprNode::Unary::Unary_t::MINUS:
      return -res;
    case ExprNode::Unary::Unary_t::BANG:
      return !res;
    }
    unreachable();
  }
  case ExprNode::Expr_t::NONE:
    throw std::runtime_error(
        std::format("Attempting to evaluate an uninitialized expression."));
  }
  unreachable();
}

auto Expressions::make_binary(allocator::Page &page, Expressions::expr_t tkn,
                              ExprNode &&lhs, ExprNode &&rhs) noexcept
    -> ExprNode {
  auto bin_t = [](expr_t tkn) {
    switch (tkn) {
    case expr_t::PLUS:
      return ExprNode::Binary::Binary_t::PLUS;
    case expr_t::MINUS:
      return ExprNode::Binary::Binary_t::MINUS;
    case expr_t::SLASH:
      return ExprNode::Binary::Binary_t::DIVIDE;
    case expr_t::STAR:
      return ExprNode::Binary::Binary_t::TIMES;
    case expr_t::GREATER:
      return ExprNode::Binary::Binary_t::GREATER;
    case expr_t::GREATER_EQ:
      return ExprNode::Binary::Binary_t::GREATER_EQ;
    case expr_t::LESS:
      return ExprNode::Binary::Binary_t::LESS;
    case expr_t::LESS_EQ:
      return ExprNode::Binary::Binary_t::LESS_EQ;
    case expr_t::BANG_EQ:
      return ExprNode::Binary::Binary_t::NEQ;
    case expr_t::EQ_EQ:
      return ExprNode::Binary::Binary_t::EQ;
    case expr_t::AND:
      return ExprNode::Binary::Binary_t::AND;
    case expr_t::OR:
      return ExprNode::Binary::Binary_t::OR;
    default:
      unreachable();
    }
  }(tkn);
  page.init();
  auto *lhs_ptr = static_cast<ExprNode *>(page.alloc(sizeof(ExprNode)));
  *lhs_ptr = std::move(lhs);
  auto *rhs_ptr = static_cast<ExprNode *>(page.alloc(sizeof(ExprNode)));
  *rhs_ptr = std::move(rhs);
  return ExprNode::from(bin_t, lhs_ptr, rhs_ptr);
}

auto Expressions::make_unary(allocator::Page &page, Expressions::expr_t tkn,
                             ExprNode &&un) noexcept -> ExprNode {
  auto un_t = [](expr_t tkn) {
    switch (tkn) {
    case expr_t::MINUS:
      return ExprNode::Unary::Unary_t::MINUS;
    case expr_t::BANG:
      return ExprNode::Unary::Unary_t::BANG;
    default:
      unreachable();
    }
  }(tkn);
  page.init();
  auto *un_ptr = static_cast<ExprNode *>(page.alloc(sizeof(ExprNode)));
  *un_ptr = std::move(un);
  return ExprNode::from(un_t, un_ptr);
}

auto Expressions::make_integer(Expressions::expr_t tkn,
                               std::string_view str) noexcept -> ExprNode {
  // TODO: idk i feel like i could do better but this is fine
  auto i = [str](expr_t tkn) -> size_t {
    switch (tkn) {
    case expr_t::LIT_DEC: {
      auto res = size_t{};
      sscanf(str.data(), "%zu", &res);
      return res;
    } break;
    case expr_t::LIT_HEX: {
      auto res = size_t{};
      sscanf(str.data(), "%zx", &res);
      return res;
    } break;
    case expr_t::LIT_CHAR:
      [[fallthrough]];
    case expr_t::LIT_OCT:
      [[fallthrough]];
    case expr_t::LIT_BIN:
      throw std::runtime_error(std::format(
          "Parsing Expr_t [{}], is not currently implimented", to_string(tkn)));
      break;
    default:
      unreachable();
    }
  }(tkn);
  return ExprNode::from(ExprNode::Integer{i});
}

auto Expressions::expand(Expressions::ExprLexer const &lexer,
                         pp::MacroMap const &macros, StringSet const &defs)
    -> ExprLexer {
  auto tkns = std::vector<expr_t>();
  auto lexes = std::vector<std::string>();
  tkns.reserve(lexer.tkns.size());
  lexes.reserve(lexer.macros.size());

  auto tkn_i = size_t{};
  auto macro_i = size_t{};

  for (; tkn_i < lexer.tkns.size();) {
    // TODO: figure out how to work with function calls
    switch (lexer.tkns[tkn_i]) {
    case expr_t::MACRO: {
      ++tkn_i;
      auto const macro_to_expand = lexer.macros[macro_i++];
      auto const [expansion_tkns, expansion_lexes] =
          expand_macro(macro_to_expand, macros, defs);

      tkns.reserve(tkns.size() + expansion_tkns.size());
      lexes.reserve(lexes.size() + expansion_lexes.size());
      for (auto i = size_t{}; i < expansion_tkns.size(); ++i) {
        tkns.push_back(expansion_tkns[i]);
      }
      for (auto i = size_t{}; i < expansion_lexes.size(); ++i) {
        lexes.push_back(expansion_lexes[i]);
      }
    } break;
    // TODO: we could just optimize this here and replace all instances of
    // defined calls, but for now i'm just trying to get something to work
    case expr_t::DEFINED: {
      tkns.push_back(expr_t::DEFINED);
      ++tkn_i;
      // NOTE: we consume any parens, so that there are no parens, this is fine
      // to do by the standard, and a small optimization(?)
      if (lexer.tkns[tkn_i] == expr_t::LPAREN) {
        ++tkn_i; // LPAREN
        tkns.push_back(expr_t::MACRO);
        ++tkn_i; // MACRO
        ++tkn_i; // RPAREN
      }
      lexes.push_back(lexer.macros[macro_i++]);
    } break;
    case expr_t::LIT_CHAR:
      [[fallthrough]];
    case expr_t::LIT_DEC:
      [[fallthrough]];
    case expr_t::LIT_HEX:
      [[fallthrough]];
    case expr_t::LIT_OCT:
      [[fallthrough]];
    case expr_t::LIT_BIN:
      [[fallthrough]];
    case expr_t::LIT_FLOAT:
      lexes.push_back(lexer.macros[macro_i++]);
      tkns.push_back(lexer.tkns[tkn_i++]);
      break;
    default: {
      tkns.push_back(lexer.tkns[tkn_i++]);
    }
    }
  }
  return ExprLexer{tkns, lexes};
}

auto Expressions::expand_macro(std::string const &macro_to_expand,
                               pp::MacroMap const &macros,
                               pp::StringSet const &def_macros) noexcept
    -> ExprLexer {
  auto tkns = std::vector<expr_t>();
  auto lexes = std::vector<std::string>();

  // NOTE: check if we need to recursively call this function until there's no
  // more macros, it might be done just in the expand function idk?
  if (auto const val = macros.find(macro_to_expand); val != macros.end()) {
    auto &&[expansion_tkns, expansion_lexes] = Expressions::lex(val->second);
    tkns = std::move(expansion_tkns);
    lexes = std::move(expansion_lexes);
  } else if (auto const val = def_macros.find(macro_to_expand);
             val != def_macros.end()) {
    // nothing to do in this case
  } else {
    tkns.push_back(expr_t::LIT_DEC);
    lexes.push_back("0");
  }
  return ExprLexer{tkns, lexes};
}

#ifdef DEBUG_CPP
auto AstPrinter::visit_if(IfNode &i) -> void {
  out << get_indents() << "(if (" << i.condition << ")" NL;
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
  out << get_indents() << ")" NL;
}

auto AstPrinter::visit_ifdef(IfDefNode &i) -> void {
  out << get_indents() << "(ifdef (" << i.macro << ")" NL;
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
  out << get_indents() << ")" NL;
}

auto AstPrinter::visit_ifndef(IfNDefNode &i) -> void {
  out << get_indents() << "(ifndef (" << i.macro << ")" NL;
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
  out << get_indents() << ")" NL;
}

auto AstPrinter::visit_elif(ElifNode &e) -> void {
  out << get_indents() << "(elif (" << e.condition << ")" NL;
  ++depth;
  for (auto &&thens : e.then_branch) {
    thens->accept(*this);
  }
  --depth;
  out << get_indents() << ")" NL;
}

auto AstPrinter::visit_else(ElseNode &e) -> void {
  out << get_indents() << "(else (" NL;
  ++depth;
  for (auto &&elses : e.stmts) {
    elses->accept(*this);
  }
  --depth;
  out << get_indents() << ")" NL;
}

auto AstPrinter::visit_global_include(GlobalIncludeNode &global) -> void {
  out << get_indents() << "(include global (" << global.path << "))" NL;
}

auto AstPrinter::visit_local_include(LocalIncludeNode &local) -> void {
  out << get_indents() << "(include local (" << local.path << "))" NL;
}

auto AstPrinter::visit_define(DefineNode &d) -> void {
  out << get_indents() << "(define (" << d.name;
  if (d.lexeme) {
    out << '{';
    out << d.lexeme.value();
    out << '}';
  }
  out << "))" NL;
}

auto AstPrinter::visit_define_func(DefineFuncNode &f) -> void {
  out << get_indents() << "(define (" << f.name << "(";
  for (auto &&param : f.parameters) {
    out << param << ",";
  }
  out << ")";
  out << "{" << f.body << "}))" NL;
}

auto AstPrinter::visit_undef(UndefNode &u) -> void {
  out << get_indents() << "(undef (" << u.name;
  out << "))" NL;
}

auto AstPrinter::visit_pragma(PragmaNode &p) -> void {
  out << get_indents() << "(pragma {" << p.value << "})" NL;
}
#endif // DEBUG_CPP

auto AstIncluder::visit_if(IfNode &i) -> void {
  if (ExprNode::eval(i.condition, alloc, macros, defs) != 0) {
    alloc.reset();
    for (auto &&thens : i.then_branch) {
      thens->accept(*this);
    }
    return;
  }
  alloc.reset();
  for (auto &&elif : i.elif_branches) {
    if (ExprNode::eval(elif->condition, alloc, macros, defs) != 0) {
      alloc.reset();
      elif->accept(*this);
      return;
    }
  }
  alloc.reset();
  if (i.else_branch != nullptr) {
    alloc.reset();
    i.else_branch->accept(*this);
  }
}

auto AstIncluder::visit_ifdef(IfDefNode &i) -> void {
  if (is_defined(i.macro, macros, defs)) {
    for (auto &&thens : i.then_branch) {
      thens->accept(*this);
    }
    return;
  }
  for (auto &&elif : i.elif_branches) {
    if (ExprNode::eval(elif->condition, alloc, macros, defs) != 0) {
      alloc.reset();
      elif->accept(*this);
      return;
    }
  }
  if (i.else_branch != nullptr) {
    i.else_branch->accept(*this);
  }
}

auto AstIncluder::visit_ifndef(IfNDefNode &i) -> void {
  if (!is_defined(i.macro, macros, defs)) {
    for (auto &&thens : i.then_branch) {
      thens->accept(*this);
    }
    return;
  }
  for (auto &&elif : i.elif_branches) {
    if (ExprNode::eval(elif->condition, alloc, macros, defs) != 0) {
      alloc.reset();
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
    defs.insert(d.name);
  }
}

auto AstIncluder::visit_define_func(DefineFuncNode &f) -> void {
  // when we fix how function macros are stored, we'll need to update this
  auto cur_format = [&f]() -> std::string {
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
  } else if (defs.contains(u.name)) {
    defs.erase(u.name);
  } else {
    //  apparently it's perfectly fine to #undef a non-existant macro, at
    //  least according to clang i should check what the docs have to say
    //  about this case
  }
}

auto AstIncluder::visit_pragma(PragmaNode &) -> void {
  return; // ? idk if there's actually anything for us to do here
}

namespace B {
// basically just a namespace with, but by doing it this way we can write
// something like `delims.`, which i like for the syntax
struct Delimiters final {
  static auto constexpr ws = std::string_view{" \t\n\r"};
  static auto constexpr define = std::string_view{" \t\n\r("};
  static auto constexpr lex_switch = std::string_view{"#/\"'"};
};
auto constexpr chars = Delimiters{};

// there are more, but for us (as far as i can tell), these are the only ones we
// care about
enum class pp_t : u8 {
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
  _EOF,
};

auto constexpr to_string(pp_t tkn) noexcept -> std::string_view {
  switch (tkn) {
  case pp_t::IF:
    return std::string_view("IF");
  case pp_t::IFDEF:
    return std::string_view("IFDEF");
  case pp_t::IFNDEF:
    return std::string_view("IFNDEF");
  case pp_t::ELIF:
    return std::string_view("ELIF");
  case pp_t::ELSE:
    return std::string_view("ELSE");
  case pp_t::ENDIF:
    return std::string_view("ENDIF");
  case pp_t::DEFINE:
    return std::string_view("DEFINE");
  case pp_t::DEFINE_FUNC:
    return std::string_view("DEFINE_FUNC");
  case pp_t::INCLUDE:
    return std::string_view("INCLUDE");
  case pp_t::UNDEF:
    return std::string_view("UNDEF");
  case pp_t::PRAGMA:
    return std::string_view("PRAGMA");
  case pp_t::_EOF:
    return std::string_view("_EOF");
  }
}

template <class T>
auto constexpr vec_append(std::vector<T> &vec, std::vector<T> &&span) noexcept
    -> void {
  auto const last = vec.end();
  vec.reserve(vec.size() + span.size());
  vec.insert(last, span.begin(), span.end());
}

template <class T>
auto constexpr nin(T obj, std::initializer_list<T> &&set) noexcept -> bool {
  return std::find(set.begin(), set.end(), obj) == set.end();
}

static auto const keywords = std::unordered_map<std::string_view, pp_t>{
    {{std::string_view{"#if"}, pp_t::IF},
     {std::string_view{"#ifdef"}, pp_t::IFDEF},
     {std::string_view{"#ifndef"}, pp_t::IFNDEF},
     {std::string_view{"#elif"}, pp_t::ELIF},
     {std::string_view{"#else"}, pp_t::ELSE},
     {std::string_view{"#endif"}, pp_t::ENDIF},
     {std::string_view{"#define"}, pp_t::DEFINE},
     {std::string_view{"#include"}, pp_t::INCLUDE},
     {std::string_view{"#undef"}, pp_t::UNDEF},
     {std::string_view{"#pragma"}, pp_t::PRAGMA}}};

struct LazyParser final {
  LazyParser(std::string_view const file) noexcept
      : buffer(file), i(0), cur_lex() {}
  LazyParser(LazyParser &&) noexcept = default;
  LazyParser &operator=(LazyParser &&) noexcept = default;

  auto get_includes(allocator::Page &, pp::MacroMap &, pp::StringSet &)
      -> std::vector<fs::path>;

  LazyParser() = delete;
  LazyParser(LazyParser const &) = delete;
  LazyParser &operator=(LazyParser const &) = delete;

private:
  std::string_view buffer;
  size_t i;
  std::string_view cur_lex;
  pp_t cur_tkn;

  // NOTE: sets the current lexeme, if the returned token corresponds with one
  // of the lex types
  auto next() -> pp_t;

  auto goto_next_branch() -> void;
  auto goto_matching_endif() -> void;
  // evaluates the current_lex
  auto eval(allocator::Page &, pp::MacroMap &, pp::StringSet &) -> int;

  auto parse_decl(std::vector<fs::path> &, allocator::Page &, pp::MacroMap &,
                  pp::StringSet &) -> void;

  auto handle_if(std::vector<fs::path> &, allocator::Page &, pp::MacroMap &,
                 pp::StringSet &) -> void;

  auto handle_ifdef(std::vector<fs::path> &, allocator::Page &, pp::MacroMap &,
                    pp::StringSet &) -> void;

  auto handle_ifndef(std::vector<fs::path> &, allocator::Page &, pp::MacroMap &,
                     pp::StringSet &) -> void;
  auto handle_define(allocator::Page &, pp::MacroMap &, pp::StringSet &)
      -> void;

  auto produce_if_arg() -> std::string_view;
};

auto LazyParser::get_includes(allocator::Page &alloc, pp::MacroMap &macros,
                              pp::StringSet &defs) -> std::vector<fs::path> {
  auto res = std::vector<fs::path>();
  for (; i < buffer.size();) {
    parse_decl(res, alloc, macros, defs);
  }
  return res;
}

// TODO
auto LazyParser::next() -> pp_t {
  for (;;) {
    if (i >= buffer.size())
      return pp_t::_EOF;
    switch (buffer[i]) {
    case '#': {
      auto end = luamake::skip_until(chars.ws, buffer, i);
      auto const hash_keyword =
          std::string_view{buffer.begin() + i, buffer.begin() + end};
      i = end;
      auto const keyword = keywords.find(hash_keyword);
      if (keyword == keywords.end())
        throw std::runtime_error(
            std::format("preprocessor directive [{}], is not a known directive",
                        hash_keyword));
      switch (keyword->second) {
      case pp_t::IF: {
        cur_lex = produce_if_arg();
        return pp_t::IF;
      } break;
      case pp_t::INCLUDE: {
        i = luamake::skip_until(chars.ws, buffer, i) + 1;
        end = luamake::skip_until('\n', buffer, i);
        cur_lex = std::string_view(buffer.begin() + i, buffer.begin() + end);
        i = luamake::skip_until(chars.lex_switch, buffer, end);
        cur_tkn = pp_t::INCLUDE;
        return pp_t::INCLUDE;
      } break;
      case pp_t::IFDEF: {
        i = luamake::skip_until(chars.ws, buffer, i) + 1;
        end = luamake::skip_until(chars.ws, buffer, i + 1);
        cur_lex = std::string_view{buffer.begin() + i, buffer.begin() + end};
        i = luamake::skip_until(chars.lex_switch, buffer, end);
        cur_tkn = pp_t::IFDEF;
        return pp_t::IFDEF;
      } break;
      case pp_t::IFNDEF: {
        i = luamake::skip_until(chars.ws, buffer, i) + 1;
        end = luamake::skip_until(chars.ws, buffer, i + 1);
        cur_lex = std::string_view{buffer.begin() + i, buffer.begin() + end};
        i = luamake::skip_until(chars.lex_switch, buffer, end);
        cur_tkn = pp_t::IFNDEF;
        return pp_t::IFNDEF;
      } break;
      case pp_t::ELSE: {
        i = luamake::skip_until(chars.lex_switch, buffer, end);
        cur_tkn = pp_t::ELSE;
        return pp_t::ELSE;
      } break;
      case pp_t::ENDIF: {
        i = luamake::skip_until(chars.lex_switch, buffer, end);
        cur_tkn = pp_t::ENDIF;
        return pp_t::ENDIF;
      } break;
      case pp_t::DEFINE: {
        // includes both define and define func, which we'll have to do at the
        // same time
        i = luamake::skip_until(chars.ws, buffer, i) + 1;
        end = luamake::skip_until(chars.define, buffer, i);
        cur_lex = std::string_view{buffer.begin() + i, buffer.begin() + end};
        if (end >= buffer.size())
          throw std::runtime_error("Unterminated #define macro");
        switch (buffer[end]) {
        case '(':
          return pp_t::DEFINE_FUNC;
        case ' ':
          [[fallthrough]];
        case '\t':
          [[fallthrough]];
        case '\r':
          [[fallthrough]];
        case '\n':
          return pp_t::DEFINE;
        default:
          unreachable();
        }
      } break;
      default:
        throw std::runtime_error(
            "idk i'm bored and want to see something happen");
      }
    }
    case '/': {
      if (i + 1 < buffer.size()) {
        ++i;
        switch (buffer[i]) {
        case '/': // single line comment
          i = luamake::skip_until('\n', buffer, i) + 1;
          break;
        case '*': // multi line comment
          for (;;) {
            i = luamake::skip_until('*', buffer, i) + 1;
            if (i < buffer.size()) {
              if (buffer[i] == '/') {
                break;
              } else {
                ++i;
              }
            } else {
              throw std::runtime_error("Unterminated multi line comment");
            }
          }
          break;
        default: // idk probably in some math expression
          i = luamake::skip_until(chars.lex_switch, buffer, i);
          break;
        }
      } else {
        // technically don't need to throw(?), but this is a formatting error,
        // but it's not something that we *need* to worry about
        throw std::runtime_error("random '/' found not connected to anything");
      }
    } break;
    case '"':
      [[fallthrough]]; // both of these cases are handled the same
    case '\'': {
      auto const ch = buffer[i];
      do {
        i = luamake::skip_until(ch, buffer, i + 1);
        if (!(i < buffer.size())) {
          throw std::runtime_error("Non terminated character literal");
        }
        // when you have a case like '\\', which does happen :)
      } while (buffer[i - 1] == '\\' && buffer[i - 2] != '\\');
      i = luamake::skip_until(chars.lex_switch, buffer, i + 1);
    } break;
    default:
      throw std::runtime_error(
          std::format("Unknown char [{}] found while lexing", buffer[i]));
    }
  }
}

// NOTE: this function makes the next call to next() do some repeated work when
// discovering the keyword and indexing into it, we should probably make it not
// do that, but for now this works
auto LazyParser::goto_next_branch() -> void {
  for (;;) {
    if (i >= buffer.size())
      throw std::runtime_error("Unable to find next branch");
    switch (buffer[i]) {
    case '#': {
      auto end = luamake::skip_until(chars.ws, buffer, i);
      auto const hash_keyword =
          std::string_view{buffer.begin() + i, buffer.begin() + end};
      auto const keyword = keywords.find(hash_keyword);
      if (keyword == keywords.end()) {
        i = luamake::skip_until(chars.lex_switch, buffer, i);
        continue;
      }
      switch (keyword->second) {
      case pp_t::ELIF:
        cur_lex = produce_if_arg();
        [[fallthrough]];
      case pp_t::ENDIF:
        [[fallthrough]];
      case pp_t::ELSE:
        // buffer[i] *should* == '#'
        cur_tkn = keyword->second;
        return;
      default:
        i = luamake::skip_until(chars.lex_switch, buffer, i);
        break;
      }
    }
    case '/': {
      if (i + 1 < buffer.size()) {
        ++i;
        switch (buffer[i]) {
        case '/': // single line comment
          i = luamake::skip_until('\n', buffer, i) + 1;
          break;
        case '*': // multi line comment
          for (;;) {
            i = luamake::skip_until('*', buffer, i) + 1;
            if (i < buffer.size()) {
              if (buffer[i] == '/') {
                break;
              } else {
                ++i;
              }
            } else {
              throw std::runtime_error("Unterminated multi line comment");
            }
          }
          break;
        default: // idk probably in some math expression
          i = luamake::skip_until(chars.lex_switch, buffer, i);
          break;
        }
      } else {
        // technically don't need to throw(?), but this is a formatting error,
        // but it's not something that we *need* to worry about
        throw std::runtime_error("random '/' found not connected to anything");
      }
    } break;
    case '"':
      [[fallthrough]]; // both of these cases are handled the same
    case '\'': {
      auto const ch = buffer[i];
      do {
        i = luamake::skip_until(ch, buffer, i + 1);
        if (!(i < buffer.size())) {
          throw std::runtime_error("Non terminated character literal");
        }
        // when you have a case like '\\', which does happen :)
      } while (buffer[i - 1] == '\\' && buffer[i - 2] != '\\');
      i = luamake::skip_until(chars.lex_switch, buffer, i + 1);
    } break;
    default:
      throw std::runtime_error(
          std::format("Unknown char [{}] found while lexing", buffer[i]));
    }
  }
  // ?
  unreachable();
}

// NOTE: this function makes the next call to next() do some repeated work when
// discovering the keyword and indexing into it, we should probably make it not
// do that, but for now this works
auto LazyParser::goto_matching_endif() -> void {
  auto depth = size_t{0};
  for (;;) {
    if (i >= buffer.size())
      return;
    switch (buffer[i]) {
    case '#': {
      auto end = luamake::skip_until(chars.ws, buffer, i);
      auto const hash_keyword =
          std::string_view{buffer.begin() + i, buffer.begin() + end};
      auto const keyword = keywords.find(hash_keyword);
      if (keyword == keywords.end()) {
        i = luamake::skip_until(chars.lex_switch, buffer, i);
        continue;
      }
      switch (keyword->second) {
      case pp_t::IF:
        [[fallthrough]];
      case pp_t::IFDEF:
        [[fallthrough]];
      case pp_t::IFNDEF:
        ++depth;
        break;
      case pp_t::ENDIF:
        if (depth == 0) {
          cur_tkn = pp_t::ENDIF;
          return;
        }
        --depth;
        break;
      default:
        i = luamake::skip_until(chars.lex_switch, buffer, i);
        break;
      }
    }
    case '/': {
      if (i + 1 < buffer.size()) {
        ++i;
        switch (buffer[i]) {
        case '/': // single line comment
          i = luamake::skip_until('\n', buffer, i) + 1;
          break;
        case '*': // multi line comment
          for (;;) {
            i = luamake::skip_until('*', buffer, i) + 1;
            if (i < buffer.size()) {
              if (buffer[i] == '/') {
                break;
              } else {
                ++i;
              }
            } else {
              throw std::runtime_error("Unterminated multi line comment");
            }
          }
          break;
        default: // idk probably in some math expression
          i = luamake::skip_until(chars.lex_switch, buffer, i);
          break;
        }
      } else {
        // technically don't need to throw(?), but this is a formatting error,
        // but it's not something that we *need* to worry about
        throw std::runtime_error("random '/' found not connected to anything");
      }
    } break;
    case '"':
      [[fallthrough]]; // both of these cases are handled the same
    case '\'': {
      auto const ch = buffer[i];
      do {
        i = luamake::skip_until(ch, buffer, i + 1);
        if (!(i < buffer.size())) {
          throw std::runtime_error("Non terminated character literal");
        }
        // when you have a case like '\\', which does happen :)
      } while (buffer[i - 1] == '\\' && buffer[i - 2] != '\\');
      i = luamake::skip_until(chars.lex_switch, buffer, i + 1);
    } break;
    default:
      throw std::runtime_error(
          std::format("Unknown char [{}] found while lexing", buffer[i]));
    }
  }
  throw std::runtime_error("idk how you got here");
  // ?
  unreachable();
}

// TODO: see if it's worth also making this lazy in some sense?
auto LazyParser::eval(allocator::Page &alloc, pp::MacroMap &macros,
                      pp::StringSet &defs) -> int {
  auto const expr_tkn = Expressions::lex(cur_lex);
  auto const expanded = Expressions::expand(expr_tkn, macros, defs);
  // see note in ExprNode::eval about malformed programs
  // TODO: handle the \n chars that are now in the string
  auto const expr_ast = expanded.to_ast(alloc);
#ifdef DEBUG_CPP
  expr_ast.display(std::cout) << std::endl;
#endif // DEBUG_CPP
  return Expressions::eval_impl(expr_ast, macros, defs);
}

// NOTE: we could possibly get away with some kind of state machine + stack,
// instead of doing this kind of parsing recursion
auto LazyParser::parse_decl(std::vector<fs::path> &res, allocator::Page &alloc,
                            pp::MacroMap &macros, pp::StringSet &defs) -> void {
  auto const cur_t = next();
  switch (cur_t) {
  case pp_t::IF:
    handle_if(res, alloc, macros, defs);
    break;
  case pp_t::IFDEF:
    handle_ifdef(res, alloc, macros, defs);
    break;
  case pp_t::IFNDEF:
    handle_ifndef(res, alloc, macros, defs);
    break;
  case pp_t::ELIF:
    [[fallthrough]];
  case pp_t::ELSE:
    [[fallthrough]];
  case pp_t::ENDIF:
    throw std::runtime_error(std::format(
        "Found {} not connected to a #if((n)?def)? preprocessor directive",
        to_string(cur_t)));
    break;
  case pp_t::DEFINE:
    handle_define(alloc, macros, defs);
    break;
  case pp_t::DEFINE_FUNC:
    throw std::runtime_error("evaluating #define not impl");
    break;
  case pp_t::INCLUDE:
    if (cur_lex[0] == '<') {
      // non-local include (global/from another module), ignoring (should
      // check that it actually exists)
    } else if (cur_lex[0] == '"') {
      // strip the wrapping '"' chars
      res.push_back(cur_lex.substr(1, cur_lex.size() - 2));
    } else {
      throw std::runtime_error(std::format(
          "Attempting to include an unknown thing(?) [{}]", cur_lex));
    }
    break;
  case pp_t::UNDEF:
    if (auto const is_macro = macros.find(cur_lex); is_macro != macros.end()) {
    } else if (auto const is_def = defs.find(cur_lex); is_def != defs.end()) {
    } else {
      // nothing to do, at least according to gcc :)
    }
    break;
  case pp_t::PRAGMA:
    // TODO: pass a map of evaluated files, because #pragma once means that
    // (as far as i can tell) even if the defined macros change we only need
    // to evaluate the file once, for now we just say whatever and eval the
    // file again :)
    break;
  case pp_t::_EOF:
    // idk maybe remove?
    break;
  default:
    unreachable();
  }
}

auto LazyParser::handle_if(std::vector<fs::path> &res, allocator::Page &alloc,
                           pp::MacroMap &macros, pp::StringSet &defs) -> void {
  if (eval(alloc, macros, defs) == 0) {
    // have to find the right branch to evaluate
    for (auto found_branch = false; found_branch != true;) {
      goto_next_branch();
      switch (cur_tkn) {
      case pp_t::ELIF:
        if (eval(alloc, macros, defs) != 0) {
          next();
          found_branch = true;
        }
        break;
      case pp_t::ELSE:
        next();
        found_branch = true;
        break;
      case pp_t::ENDIF: // should probably advance the token?
        next();
        return; // found #endif, with nothing in between that we could use
        break;
      case pp_t::_EOF:
        throw std::runtime_error("Unterminated #if preprocessor directive");
      default:
        unreachable();
      }
    }
  }

  for (auto got_all = false; got_all != true;) {
    auto const cur = next();
    switch (cur) {
    case pp_t::IF:
      handle_if(res, alloc, macros, defs);
      break;
    case pp_t::IFDEF:
      handle_ifdef(res, alloc, macros, defs);
      break;
    case pp_t::IFNDEF:
      handle_ifndef(res, alloc, macros, defs);
      break;
    case pp_t::ELIF:
      [[fallthrough]];
    case pp_t::ELSE:
      [[fallthrough]];
    case pp_t::ENDIF:
      got_all = true;
      break;
    case pp_t::DEFINE:
      handle_define(alloc, macros, defs);
      break;
    case pp_t::DEFINE_FUNC: {
      throw std::runtime_error("evaluating #define not impl");
    } break;
    case pp_t::INCLUDE: {
      if (cur_lex[0] == '<') {
        // non-local include (global/from another module), ignoring (should
        // check that it actually exists)
      } else if (cur_lex[0] == '"') {
        // strip the wrapping '"' chars
        res.push_back(cur_lex.substr(1, cur_lex.size() - 2));
      } else {
        throw std::runtime_error(std::format(
            "Attempting to include an unknown thing(?) [{}]", cur_lex));
      }
    } break;
    case pp_t::UNDEF: {
      if (auto const is_macro = macros.find(cur_lex);
          is_macro != macros.end()) {
        macros.erase(is_macro);
      } else if (auto const is_def = defs.find(cur_lex); is_def != defs.end()) {
        defs.erase(is_def);
      } else {
        // nothing to do, at least according to gcc :)
      }
    } break;
    case pp_t::PRAGMA: {
      // TODO: pass a map of evaluated files, because #pragma once means that
      // (as far as i can tell) even if the defined macros change we only need
      // to evaluate the file once, for now we just say whatever and eval the
      // file again :)
    } break;
    case pp_t::_EOF: {
      // idk maybe remove?
    } break;
    default:
      unreachable();
    }
  }
  goto_matching_endif();
  if (cur_tkn != pp_t::ENDIF)
    throw std::runtime_error(
        "Expected #endif to wrap #if preprocessor directive");
}

auto LazyParser::handle_ifdef(std::vector<fs::path> &res,
                              allocator::Page &alloc, pp::MacroMap &macros,
                              pp::StringSet &defs) -> void {
  // have to find the right branch to get the values from
  if (!is_defined(cur_lex, macros, defs)) {
    for (auto found_branch = false; !found_branch;) {
      goto_next_branch();
      switch (cur_tkn) {
      case pp_t::ELIF:
        if (eval(alloc, macros, defs) != 0)
          found_branch = true;
        break;
      case pp_t::ELSE:
        found_branch = true;
        break;
      case pp_t::ENDIF:
        // early return, nothing to do
        return;
      case pp_t::_EOF:
        throw std::runtime_error("Unterminated #ifndef macro");
      default:
        unreachable();
      }
    }
  }

  while (nin(cur_tkn, {pp_t::ELIF, pp_t::ELSE, pp_t::ENDIF})) {
    auto const cur = next();
    switch (cur) {
    case pp_t::IF:
      handle_if(res, alloc, macros, defs);
      break;
    case pp_t::IFDEF:
      handle_ifdef(res, alloc, macros, defs);
      break;
    case pp_t::IFNDEF:
      handle_ifndef(res, alloc, macros, defs);
      break;
    case pp_t::ELIF:
      [[fallthrough]];
    case pp_t::ELSE:
      [[fallthrough]];
    case pp_t::ENDIF:
      break;
    case pp_t::DEFINE:
      handle_define(alloc, macros, defs);
      break;
    case pp_t::DEFINE_FUNC:
      throw std::runtime_error("Not impl, don't want to worry about this yet");
      break;
    case pp_t::INCLUDE:
      if (cur_lex[0] == '<') {
        // non-local include (global/from another module), ignoring (should
        // check that it actually exists)
      } else if (cur_lex[0] == '"') {
        // strip the wrapping '"' chars
        res.push_back(cur_lex.substr(1, cur_lex.size() - 2));
      } else {
        throw std::runtime_error(std::format(
            "Attempting to include an unknown thing(?) [{}]", cur_lex));
      }
      break;
    case pp_t::UNDEF:
      [[fallthrough]];
    case pp_t::PRAGMA:
      throw std::runtime_error("not impl");
      break;
    case pp_t::_EOF:
      throw std::runtime_error(
          "Unterminated branch of #ifndef preprocessor directive");
    default:
      unreachable();
    }
  }
  goto_matching_endif();
}

// we're just handling the most basic case to get this working and see the kinks
auto LazyParser::handle_ifndef(std::vector<fs::path> &res,
                               allocator::Page &alloc, pp::MacroMap &macros,
                               pp::StringSet &defs) -> void {
  // have to find the right branch to get the values from
  if (is_defined(cur_lex, macros, defs)) {
    for (auto found_branch = false; !found_branch;) {
      goto_next_branch();
      switch (cur_tkn) {
      case pp_t::ELIF:
        if (eval(alloc, macros, defs) != 0)
          found_branch = true;
        break;
      case pp_t::ELSE:
        found_branch = true;
        break;
      case pp_t::ENDIF:
        // early return, nothing to do
        return;
      case pp_t::_EOF:
        throw std::runtime_error("Unterminated #ifndef macro");
      default:
        unreachable();
      }
    }
  }

  while (nin(cur_tkn, {pp_t::ELIF, pp_t::ELSE, pp_t::ENDIF})) {
    auto const cur = next();
    switch (cur) {
    case pp_t::IF:
      handle_if(res, alloc, macros, defs);
      break;
    case pp_t::IFDEF:
      handle_ifdef(res, alloc, macros, defs);
      break;
    case pp_t::IFNDEF:
      handle_ifndef(res, alloc, macros, defs);
      break;
    case pp_t::ELIF:
      [[fallthrough]];
    case pp_t::ELSE:
      [[fallthrough]];
    case pp_t::ENDIF:
      break;
    case pp_t::DEFINE:
      handle_define(alloc, macros, defs);
      break;
    case pp_t::DEFINE_FUNC:
      throw std::runtime_error("Not impl, don't want to worry about this yet");
      break;
    case pp_t::INCLUDE:
      if (cur_lex[0] == '<') {
        // non-local include (global/from another module), ignoring (should
        // check that it actually exists)
      } else if (cur_lex[0] == '"') {
        // strip the wrapping '"' chars
        res.push_back(cur_lex.substr(1, cur_lex.size() - 2));
      } else {
        throw std::runtime_error(std::format(
            "Attempting to include an unknown thing(?) [{}]", cur_lex));
      }
      break;
    case pp_t::UNDEF:
      [[fallthrough]];
    case pp_t::PRAGMA:
      throw std::runtime_error("not impl");
      break;
    case pp_t::_EOF:
      throw std::runtime_error(
          "Unterminated branch of #ifndef preprocessor directive");
    default:
      unreachable();
    }
  }
  goto_matching_endif();
}

auto LazyParser::handle_define(allocator::Page &alloc, pp::MacroMap &macros,
                               pp::StringSet &defs) -> void {
  auto const macro_name = cur_lex;

  // need to do some lexing ourselves because we don't currently support this
  auto const end = luamake::skip_until(chars.define, buffer, i);
  if (end >= buffer.size())
    throw std::runtime_error("Unterminated #define macro");
  switch (buffer[end]) {
  case ' ':
    [[fallthrough]];
  case '\t': // #define FOO <expr>
    throw std::runtime_error(
        "Currently do not support defining macros with values");
    break;
  case '\r':
    [[fallthrough]];
  case '\n': // #define FOO
    defs.insert(macro_name);
    break;
  case '(': // #define FOO()
    throw std::runtime_error(
        "Currently do not support parsing function macros");
    break;
  default:
    unreachable();
  }
  i = luamake::skip_until(chars.lex_switch, buffer, end);
}

auto LazyParser::produce_if_arg() -> std::string_view {
  i = luamake::skip_until(chars.ws, buffer, i) + 1;
  if (i >= buffer.size())
    throw std::runtime_error("Unterminated #if preprocessor directive");
  if (buffer[i - 1] == '\n')
    throw std::runtime_error("Empty #if directive");
  auto end = i;
  do {
    end = luamake::skip_until('\n', buffer, end + 1);
    if (end >= buffer.size())
      throw std::runtime_error(
          "Unterminated #if preprocessor directive expression");
  } while (buffer[end - 1] == '\\');
  auto const res = std::string_view{buffer.begin() + i, buffer.begin() + end};
  i = luamake::skip_until(chars.lex_switch, buffer, end);
  return res;
}

auto get_includes(std::string_view const file, allocator::Page &alloc,
                  pp::MacroMap &macros, pp::StringSet &defs)
    -> std::vector<fs::path> {
  auto par = LazyParser(file);
  return par.get_includes(alloc, macros, defs);
}
} // namespace B
} // namespace

auto Interpreter::interpret(std::string_view const file, allocator::Page &alloc)
    -> std::vector<fs::path> {
  auto ast = Lexer::lex(file).ast();
  auto vec = std::vector<fs::path>();
#ifdef DEBUG_CPP
  auto ast_p = AstPrinter(std::cout);
  ast_p.print(ast);
#endif // DEBUG_CPP
  auto includer = AstIncluder(vec, alloc, macros, defs);
  includer.get_includes(ast);
#ifdef DEBUG_CPP
  // technically not A, but this is just for some idea of ab testing
  std::cout << "files from the A\n";
  for (auto &&f : vec) {
    std::cout << f << '\n';
  }
  std::cout << "---\n";

  auto macros_b = pp::MacroMap();
  auto defs_b = pp::StringSet();
  auto const test = B::get_includes(file, alloc, macros_b, defs_b);
  std::cout << "files from the B\n";
  for (auto &&f : test) {
    std::cout << f << '\n';
  }
  std::cout << "---\n";
  return test;
#endif // DEBUG
  return vec;
}

#ifdef DEBUG_CPP
auto Interpreter::dump_macros(std::ostream &out) noexcept -> void {
  out << "macros = {" NL;
  for (auto &&[name, value] : macros) {
    out << name << "=" << value << "," NL;
  }
  out << "}" NL;

  out << "defined_macros = ";
  out << "[" << defs.size() << "]{" NL;
  for (auto const &name : defs) {
    out << name << "," NL;
  }
  out << "}" NL;
}
#endif // DEBUG_CPP
} // namespace luamake::pp
