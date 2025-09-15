#include "luamake_pre_ir.hpp"
#include "luamake_strings.hpp"

#include <cctype>
#include <cstring>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef DEBUG
#include <iostream>
#endif // DEBUG

namespace fs = std::filesystem;

using std::string, std::string_view, std::vector, std::unordered_map;

namespace luamake {
namespace ir {
namespace {
auto constexpr string_len(char const *chars) -> size_t {
  auto size = size_t{};
  while (chars[size] != 0)
    ++size;
  return size;
}

auto constexpr skip_ws(size_t const size, char const *const buf, size_t i)
    -> size_t {
  auto constexpr *ws = " \t\n\r";
  auto constexpr ws_len = string_len(ws);
  while (i < size) {
    for (auto idx = size_t{}; idx < ws_len; ++idx) {
      if (buf[i] == ws[idx]) {
        return i;
      }
    }
    ++i;
  }

  return i;
}

// template <char const *delims>
auto constexpr skip_until(string_view const delims, size_t const size,
                          char const *buffer, size_t i) -> size_t {
  while (i < size) {
    for (auto idx = size_t{}; idx < delims.size(); ++idx) {
      if (delims[idx] == buffer[i]) {
        return i;
      }
    }
    ++i;
  }
  return i;
}

template <char delim>
auto constexpr skip_until(size_t const size, char const *const buffer, size_t i)
    -> size_t {
  while (i < size) {
    if (delim == buffer[i])
      return i;
    ++i;
  }
  return i;
}

auto constexpr skip_until_close_multicomment(size_t const size,
                                             char const *const buffer, size_t i)
    -> size_t {
  while (i < size && i + 1 < size) {
    if (buffer[i] == '*' && buffer[i + 1] == '/') {
      return i + 2; // put buffer[i + 2 - 1] == '/'
    } else {
      ++i;
    }
  }
  return size;
}

template <class T>
auto constexpr skip_until(size_t i, size_t const size, T const *const buffer,
                          T delim) -> size_t {
  while (i < size) {
    if (buffer[i] == delim) {
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

// i would like to add lexical short cutting, where if we see a macro that's
// already been defined in something like a header guard, then we completely
// skip the file
// TODO: there's actually a lot more we need to do for macro preprocessing, like
// properly parsing expressions (adding them to the lexer :))
// TODO: add proper lexing to the preprocessor, i think that for #if expressions
// we just need to lex until we hit a '\n' character, then when we're parsing we
// can form an expression tree, thankfully everything must eventually be
// interpreted as an int (0 == false, x == true), so we can just have our
// interpreter worry about int's and their expressions, how they get converted
// to int's etc
auto PP_Lexer::lex(FixedString const &file) -> PP_Lexer {
  // clang-format off
  static auto const keywords = unordered_map<string_view, enum PP_Lexer::types>{{
    {string_view{"#if"}, IF},
    {string_view{"#ifdef"}, IFDEF},
    {string_view{"#ifndef"}, IFNDEF},
    {string_view{"#elif"}, ELIF},
    {string_view{"#else"}, ELSE},
    {string_view{"#endif"}, ENDIF},
    {string_view{"#define"}, DEFINE},
    {string_view{"#include"}, INCLUDE},
    {string_view{"defined"}, OP_DEFINED},
    {string_view{"#undef"}, UNDEF},
    {string_view{"#pragma"}, PRAGMA}
  }};
  auto constexpr chars_of_interest = string_view{"#/\""};
  // clang-format on
  auto lex = PP_Lexer();

  auto const fcontent = file.buffer;
  for (auto i = size_t{}; i < file.size;) {
    switch (fcontent[i]) {
    case '#': {
      auto end = skip_until(" \t\r\n", file.size, fcontent, i);

      auto const hash_keyword = string_view{fcontent + i, fcontent + end};
      i = end;
      auto const keyword = keywords.find(hash_keyword);
      if (keyword == keywords.end()) {
        // continue to next character of interest
        i = skip_until(chars_of_interest, file.size, fcontent, i);
        throw Lex_Exc(
            __LINE__,
            std::format("Hash keyword [{}], is not implimented", hash_keyword));
      }

      switch (keyword->second) {
      case INCLUDE: {
        if (i >= file.size)
          throw Lex_Exc(__LINE__, string("Unable to parse include parameter"));

        i = skip_ws(file.size, fcontent, i) + 1;

        if (i >= file.size)
          throw Lex_Exc(__LINE__, string("Unable to parse include parameter"));

        lex.types.emplace_back(INCLUDE);
        switch (fcontent[i]) {
        case '<': {
          lex.types.emplace_back(LANGLE);

          end = skip_until<'>'>(file.size, fcontent, i + 1);

          if (!(end < file.size))
            throw Lex_Exc(__LINE__, string("Non terminated global include"));

          lex.types.emplace_back(LIT_STRING);
          lex.lexemes.emplace_back(
              string_view{fcontent + i + 1, fcontent + end});
          i = end + 1;
          lex.types.emplace_back(RANGLE);
        } break;
        case '"': {
          lex.types.emplace_back(QUOTE);

          end = skip_until<'"'>(file.size, fcontent, i + 1);

          if (!(end < file.size))
            throw Lex_Exc(__LINE__, string("Non terminated local include"));

          lex.types.emplace_back(LIT_STRING);
          lex.lexemes.emplace_back(
              string_view{fcontent + i + 1, fcontent + end});
          i = end + 1;
          lex.types.emplace_back(QUOTE);
        } break;
        default:
          throw Lex_Exc(__LINE__,
                        std::format("character found = {}", fcontent[i]));
        }
      } break;
      case IFDEF: {
        lex.types.push_back(IFDEF);
        i = skip_ws(file.size, fcontent, i) + 1;
        end = skip_until(" \t\n\r", file.size, fcontent, i + 1);
        lex.types.push_back(LEXEME);
        lex.lexemes.emplace_back(string_view{fcontent + i, fcontent + end});
      } break;
      case IFNDEF: {
        lex.types.push_back(IFNDEF);
        i = skip_ws(file.size, fcontent, i) + 1;
        end = skip_until(" \t\n\r", file.size, fcontent, i + 1);
        lex.types.push_back(LEXEME);
        lex.lexemes.emplace_back(string_view{fcontent + i, fcontent + end});
      } break;
      case DEFINE: { // TODO: handle the case when we have a macro function, and
                     // when we're defining a macro with a value attached to it
        lex.types.push_back(DEFINE);
        i = skip_ws(file.size, fcontent, i) + 1;
        end = skip_until(" \t\n\r", file.size, fcontent, i + 1);
        lex.types.push_back(LEXEME);
        lex.lexemes.emplace_back(string_view{fcontent + i, fcontent + end});
      } break;
      case UNDEF: {
        lex.types.push_back(UNDEF);
        i = skip_ws(file.size, fcontent, i) + 1;
        end = skip_until(" \t\n\r", file.size, fcontent, i + 1);
        lex.types.push_back(LEXEME);
        lex.lexemes.emplace_back(string_view{fcontent + i, fcontent + end});
      } break;
      case IF:
        lex.types.push_back(IF);
        i = skip_ws(file.size, file.buffer, i) + 1;
        i = lex.handle_hashif(file.size, fcontent, i);
        break;
      default:
        lex.types.push_back(keyword->second);
        break;
      }
    } break;
    case '/': {
      if (!(i + 1 < file.size)) {
        throw Lex_Exc(__LINE__, string("'/' found at end of file"));
      }
      ++i;
      switch (fcontent[i]) {
      case '/': { // skip until \n
        i = skip_until<'\n'>(file.size, fcontent, i + 1);
      } break;
      case '*': { // skip until */
        // TODO: check this code, there might be an issue if the file
        // ends with a multi line comment, i.e. */ at the end of the file
        i = skip_until_close_multicomment(file.size, fcontent, i + 1);
        if (i == file.size)
          throw Lex_Exc(__LINE__, string("Non terminated multi line comment"));
      } break;
      default: // probably just an op /
        i = skip_until("#/\"", file.size, fcontent, i + 1);
        break;
      }
    } break;
    case '"': {
      i = skip_until<'"'>(file.size, fcontent, i + 1) + 1;
      if (!(i < file.size))
        throw Lex_Exc(__LINE__, string("Non terminated string"));
    } break;
    default:
      i = skip_until("#/\"", file.size, fcontent, i + 1);
      break;
    }
  }

  return lex;
}

auto PP_Lexer::parse_to_ast() -> IR_AST {
  auto ir = IR_AST();
  auto cur_t = size_t{};
  auto cur_lex = size_t{};
  while (cur_t < types.size()) {
    ir.check_size();
    switch (types[cur_t]) {
    case INCLUDE:
      ++cur_t;
      switch (types[cur_t]) {
      case LANGLE:
        cur_t += 3; // LANGLE CHAR_LIT RANGLE
        ir.push(IR_AST::IR_Types::GLOBAL_INCLUDE,
                ir.make_charlit(lexemes[cur_lex++]));
        break;
      case QUOTE:
        cur_t += 3; // QUOTE CHAR_LIT QUOTE
        ir.push(IR_AST::IR_Types::LOCAL_INCLUDE,
                ir.make_charlit(lexemes[cur_lex++]));
        break;
      default:
        throw Lex_Exc(__LINE__, std::format("Malformed #include statement, "
                                            "expected '<' or '\"', found [{}]",
                                            to_string(types[cur_t])));
      }
      break;
    case IFDEF:
      cur_t += 2;
      ir.push(IR_AST::IR_Types::IFDEF, ir.make_charlit(lexemes[cur_lex++]));
      break;
    case IFNDEF:
      cur_t += 2;
      ir.push(IR_AST::IR_Types::IFNDEF, ir.make_charlit(lexemes[cur_lex++]));
      break;
    case ENDIF:
      ++cur_t;
      ir.push(IR_AST::IR_Types::ENDIF, nullptr);
      break;
    case ELSE:
      ++cur_t;
      ir.push(IR_AST::IR_Types::ELSE, nullptr);
      break;
    case DEFINE:
      cur_t += 2;
      ir.push(IR_AST::IR_Types::DEFINE, ir.make_charlit(lexemes[cur_lex++]));
      break;
    default:
      throw Lex_Exc(__LINE__, std::format("Not implimented, type = [{}]",
                                          to_string(types[cur_t])));
      break;
    }
  }
  return ir;
}

// TODO: extract all of these delim strings into variables that we can check on
auto PP_Lexer::handle_hashif(size_t const size, char const *const buffer,
                             size_t i) -> size_t {
  auto constexpr defined_str = string_view{"defined"};
  auto looping = true;
  while (looping && i < size) {
    auto const ch = buffer[i];
    switch (ch) {
    case '\\': {
      ++i;
      if (i < size && buffer[i] == '\n') {
        ++i;
      }
    } break;
    case '\n': {
      looping = false;
      ++i;
    } break;
    case '(': {
      types.push_back(LPAREN);
      ++i;
    } break;
    case ')': {
      types.push_back(RPAREN);
      ++i;
    } break;
    case '|': {
      ++i;
      if (i < size && buffer[i] == '|') {
        ++i;
        types.push_back(OP_OR);
      } else {
        types.push_back(OP_BIT_OR);
      }
    } break;
    case '&': {
      ++i;
      if (i < size && buffer[i] == '&') {
        ++i;
        types.push_back(OP_AND);
      } else {
        types.push_back(OP_BIT_AND);
      }
    } break;
    case '!': {
      ++i;
      types.push_back(BANG);
    } break;
    case '<': {
      ++i;
      if (i < size && buffer[i] == '=') {
        ++i;
        types.push_back(OP_LESS_EQUAL);
      } else {
        types.push_back(OP_LESS);
      }
    } break;
    case '>': {
      ++i;
      if (i < size && buffer[i] == '=') {
        ++i;
        types.push_back(OP_GREATER_EQUAL);
      } else {
        types.push_back(OP_GREATER);
      }
    } break;
    case '#': {
      ++i;
      if (i < size && buffer[i] == '#') {
        ++i;
        types.push_back(OP_STRINGIZING);
      } else {
        types.push_back(OP_CONCAT);
      }
    } break;
    case 'd': {
      if (i + defined_str.size() < size &&
          strncmp(buffer + i, defined_str.data(), defined_str.size()) == 0) {
        i += defined_str.size();
        types.push_back(OP_DEFINED);
      } else {
        auto const start = i;
        i = skip_until(delims_at(delims::LEXEME), size, buffer, i);
        types.push_back(LEXEME);
        lexemes.emplace_back(string_view{buffer + start, buffer + i});
      }
    } break;
#pragma region octal
    case '0': {
      ++i;
      if (!(i < size)) {
        types.push_back(LIT_INT);
        lexemes.push_back("0");
      } else {
        switch (buffer[i]) {
#pragma region binary
        case 'b': {
          // TODO: check if we're on c++14>=, bc otherwise this is supposed to
          // be an error, the same goes with "'" character
          ++i;
          if (!(i < size)) {
            throw; // TODO: misformed octal number + additional information
                   // about int formatting
          }
          auto const start = i - 1;
          auto allow_quote = false;
          auto inner_looping = true;
          while (inner_looping && i < size) {
            switch (buffer[i]) {
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
                throw; // malformed number, double '' found
              }
            } break;
            default: {
              if (is_any_of(delims_at(delims::BINARY_FAIL), buffer, i)) {
                throw; // malformed number
              } else if (std::isalpha(buffer[i])) {
                throw; // malformed input(?) kinda a different error
              }
              inner_looping = false;
            }
            }
          }
          types.push_back(LIT_BINARY);
          lexemes.emplace_back(string_view{buffer + start, buffer + i});
        } break;
#pragma endregion binary
#pragma region hex
        case 'x': {
          ++i;
          if (!(i < size)) {
            throw; // TODO: misformed octal number + additional information
                   // about int formatting
          }
          auto const start = i - 1;
          auto allow_quote = false;
          auto inner_looping = true;
          while (inner_looping && i < size) {
            if (is_any_of(delims_at(delims::ALLOWED_HEX), buffer, i)) {
              allow_quote = true;
              ++i;
            } else if (buffer[i] == '\'') {
              if (allow_quote) {
                ++i;
                allow_quote = false;
              } else {
                throw; // malformed number, double ' found
              }
            } else if (is_any_of(delims_at(delims::ALPHA_SANS_HEX), buffer,
                                 i)) {
              throw; // malformmed number
            } else {
              inner_looping = false;
            }
          }
          types.push_back(LIT_HEX);
          lexemes.emplace_back(string_view{buffer + start, buffer + i});
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
          while (i < size) {
            if (is_any_of(delims_at(delims::ALLOWED_OCTAL), buffer, i)) {
              allow_quote = true;
              ++i;
            } else if (buffer[i] == '\'') {
              if (allow_quote) {
                allow_quote = false;
                ++i;
              } else {
                throw; // malformed octal number
              }
            } else {
              if (is_any_of(delims_at(delims::LEXEME), buffer, i)) {
                break;
              } else {
                throw; // malformed octal
              }
            }
          }
          types.push_back(LIT_OCTAL);
          lexemes.emplace_back(string_view{buffer + start, buffer + i});
        } break;
        case '8':
          [[fallthrough]];
        case '9': {
          // TODO: throw a malformed int expression
          throw; // malformed octal
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
      while (i < size) {
        if (is_any_of(delims_at(delims::DECIMAL_DIGITS), buffer, i)) {
          allow_quote = true;
          ++i;
        } else if (buffer[i] == '\'') {
          if (allow_quote) {
            allow_quote = false;
            ++i;
          } else {
            throw; // malformed decimal number
          }
        } else if (is_any_of(delims_at(delims::HEX_SANS_DIGITS), buffer, i)) {
          throw; // malformed decimal, trying to treat the number as hex
        } else {
          if (is_any_of(delims_at(delims::LEXEME), buffer, i)) {
            break;
          } else {
            throw; // malformed decimal
          }
        }
      }
      types.push_back(LIT_INT);
      lexemes.emplace_back(string_view{buffer + start, buffer + i});
    } break;
#pragma endregion decimal
    default: {
      auto const start = i;
      i = skip_until(delims_at(delims::LEXEME), size, buffer, i);
      types.push_back(LEXEME);
      lexemes.emplace_back(string_view{buffer + start, buffer + i});
    } break;
    }
  }
  return i;
}

#ifdef DEBUG
auto PP_Lexer::display(std::ostream &out) const noexcept -> std::ostream & {
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

auto Lex_Exc::what() const noexcept -> std::string {
  return std::format("Lex Error on line {}\n\tAdditional info: [{}]", line,
                     message);
}

auto Parse_Exc::what() const noexcept -> string {
  return std::format("Parse Error on line {}\n\tAdditional info: {}", line,
                     message);
}

auto Interpret_Exc::what() const noexcept -> string {
  return std::format("Interpreter Error on line {}\n\tAdditional info: {}",
                     line, message);
}

IR_AST::IR_AST()
    : size(0), cap(8), ast(std::make_unique<IR_Types[]>(cap)),
      exprs(std::make_unique<std::unique_ptr<Expr_Node>[]>(cap)) {}

auto parse(FixedString const &file) -> IR_AST {
#ifdef DEBUG
  auto lexer = PP_Lexer::lex(file);
  lexer.display(std::cerr);
  return lexer.parse_to_ast();
#else
  return PP_Lexer::lex(file).parse_to_ast();
#endif
}

auto IR_AST::check_size() -> void {
  if (size == cap) {
    auto const new_cap = cap * 2;
    auto new_ast = std::make_unique<IR_AST::IR_Types[]>(new_cap);
    auto new_exprs = std::make_unique<std::unique_ptr<Expr_Node>[]>(new_cap);

    std::memmove(new_ast.get(), ast.get(), sizeof(enum IR_AST::IR_Types) * cap);
    for (auto i = size_t{}; i < cap; ++i) {
      new_exprs[i] = std::move(exprs[i]);
    }
    ast = std::move(new_ast);
    exprs = std::move(new_exprs);
    cap = new_cap;
  }
}

auto IR_AST::push(IR_AST::IR_Types t, std::unique_ptr<Expr_Node> &&expr)
    -> void {
  ast[size] = t;
  exprs[size] = std::move(expr);
  ++size;
}

auto IR_AST::make_charlit(std::string_view const sv)
    -> std::unique_ptr<Expr_Node> {
  auto const start = static_cast<unsigned int>(lexemes.size);
  lexemes.append(sv);
  auto const end = static_cast<unsigned int>(lexemes.size);

  auto res = std::make_unique<CharLit>(StringViews{start, end});
  return nullptr;
}

auto IR_Interpreter::interpret(IR_AST const &ir) -> vector<fs::path> {
  auto vec = vector<fs::path>();
  for (auto i = size_t{}; i < ir.size;) {
    interpret_impl(false, ir, i, vec);
  }
  return vec;
}

auto IR_Interpreter::interpret_impl(bool interpret_elses, IR_AST const &ir,
                                    size_t &i, vector<fs::path> &vec) -> void {
  auto constexpr terminating_nodes = std::array<IR_AST::IR_Types, 3>{
      {IR_AST::IR_Types::ENDIF, IR_AST::IR_Types::ELSE,
       IR_AST::IR_Types::ELIF}};
#ifdef DEBUG
  std::cerr << "\tLooking at [" << IR_AST::pretty_types(ir.ast[i]) << "]\n";
#endif // DEBUG
  switch (ir.ast[i]) {
  case IR_AST::IR_Types::GLOBAL_INCLUDE: {
    // TODO: check that this file *actually exists*
    ++i;
  } break;
  case IR_AST::IR_Types::LOCAL_INCLUDE: {
    auto const lit = ((CharLit *)(ir.exprs.get() + i))->lit;
    auto const include_name = string(string_view{ir.lexemes.buffer + lit.start,
                                                 ir.lexemes.buffer + lit.end});
    vec.push_back(include_name);
  } break;
  case IR_AST::IR_Types::IF: {
    ++i;
    auto const expr = eval(ir, i);
    if (expr == 1) {
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
    }
  } break;
  case IR_AST::IR_Types::IFDEF: {
    auto const lit = ((CharLit *)(ir.exprs.get() + i))->lit;
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
        throw Interpret_Exc(__LINE__, "Unterminated #ifdef expression");
      i = skip_until(i, ir.size, ir.ast.get(), IR_AST::IR_Types::ENDIF);
      if (i == ir.size)
        throw Interpret_Exc(__LINE__, "Unterminated #ifdef expression");
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
        throw Interpret_Exc(__LINE__, "Unterminated #ifdef expression");

      interpret_impl(true, ir, i, vec);

      i = skip_until(i, ir.size, ir.ast.get(), IR_AST::IR_Types::ENDIF);
      if (i == ir.size) {
        throw Interpret_Exc(__LINE__, "Unterminated #ifdef expression");
      }
      ++i; // move over the #endif
    }
  } break;
  case IR_AST::IR_Types::IFNDEF: {
    auto const lit = ((CharLit *)(ir.exprs.get() + i))->lit;
    auto const checking_macro = string(string_view{
        ir.lexemes.buffer + lit.start, ir.lexemes.buffer + lit.end});
    auto const defined =
        macros.contains(checking_macro) || def_macros.contains(checking_macro);
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
        throw Interpret_Exc(__LINE__, "Unterminated #ifndef expression");
      i = skip_until(i, ir.size, ir.ast.get(), IR_AST::IR_Types::ENDIF);
      if (i == ir.size)
        throw Interpret_Exc(__LINE__, "Unterminated #ifndef expression");
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
        throw Interpret_Exc(__LINE__, "Unterminated #ifndef expression");

      if (ir.ast[i] == IR_AST::IR_Types::ENDIF) {
        ++i;
        return;
      }
      interpret_impl(true, ir, i, vec);

      i = skip_until(i, ir.size, ir.ast.get(), IR_AST::IR_Types::ENDIF);
      if (i == ir.size) {
        throw Interpret_Exc(__LINE__, "Unterminated #ifndef expression");
      }
    }
  } break;
  case IR_AST::IR_Types::ELSE: {
    if (!interpret_elses) {
      throw Interpret_Exc(__LINE__, "Unsupported interpretation of #else, "
                                    "possibly misformatted preprocessor");
    } else {
      ++i;
      while (i < ir.size) {
        if (ir.ast[i] == IR_AST::IR_Types::ENDIF) {
          break;
        } else {
          interpret_impl(false, ir, i, vec);
        }
      }
    }
  } break;
  case IR_AST::IR_Types::DEFINE: {
    // TODO: parse object like macros
    // NOTE: this should be a dynamic_cast, but we have the fno-rtti flag, so we
    // have to do the conversions ourselves
    auto const lit = ((CharLit *)(ir.exprs.get() + i))->lit;
    auto const defining_macro = string(string_view{
        ir.lexemes.buffer + lit.start, ir.lexemes.buffer + lit.end});
    def_macros.emplace(defining_macro);
  } break;
  default:
    throw Interpret_Exc(__LINE__, std::format("Not implimented, type = [{}]",
                                              IR_AST::pretty_types(ir.ast[i])));
  }
}

// TODO
auto IR_Interpreter::eval(IR_AST const &ir, size_t &i) const -> int {
  return 0;
}

auto IR_Interpreter::search_for_next_scope(IR_AST const &ir, size_t i) const
    -> size_t {
  auto constexpr terminating_nodes = std::array<IR_AST::IR_Types, 3>{
      {IR_AST::IR_Types::ENDIF, IR_AST::IR_Types::ELSE,
       IR_AST::IR_Types::ELIF}};
  while (i < ir.size) {
    if (std::any_of(
            terminating_nodes.begin(), terminating_nodes.end(),
            [node = ir.ast[i]](auto &&cur_node) { return cur_node == node; })) {
      break;
    } else {
      ++i;
    }
  }
  return i;
}
} // namespace ir
} // namespace luamake
