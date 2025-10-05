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
#include <unordered_set>
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
namespace ir {
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

constexpr auto find_next_non_alpha(string_view const sv, size_t idx) -> size_t {
  auto constexpr alphabet =
      string_view{"dabcefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"};
  while (idx < sv.size()) {
    if (alphabet.find(sv[idx]) != alphabet.npos) {
      ++idx;
    }
  }

  return idx;
}

constexpr auto contains_lexemes(string_view const sv)
    -> std::pair<bool, std::pair<size_t, size_t>> {
  // reordered the alphabet to be a bit better to work with
  auto constexpr alphabet =
      string_view{"dabcefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"};
  auto constexpr defined = string_view{"defined"};
  auto idx = sv.find_first_of(alphabet);
  while (idx != sv.npos) {
    if (sv[idx] == 'd') {
      // possibly the defined string
      auto const possible_lex = sv.substr(idx, defined.size());
      if (possible_lex == defined) {
        // skip over the lexeme
        idx = sv.find_first_of(alphabet, idx + defined.size());
      } else {
        return {true, {idx, find_next_non_alpha(sv, idx)}};
      }
    } else {
      return {true, {idx, find_next_non_alpha(sv, idx)}};
    }
  }
  return {false, {0, 0}};
}

static_assert(contains_lexemes(string_view{"alpha"}).first);
static_assert(!contains_lexemes(string_view{"defined"}).first);
static_assert(contains_lexemes(string_view{"1 + 2 * SIZE"}).first);
static_assert(contains_lexemes(string_view{"1 + 2 * SIZE"}).second.first == 8);
static_assert(contains_lexemes(string_view{"1 + 2 * SIZE"}).second.second ==
              12);
static_assert(string_view{"1 + 2 * SIZE"}.size() == 12);
static_assert(!contains_lexemes(string_view{"defined(LIB1)"}).first);
} // namespace

// i would like to add lexical short cutting, where if we see a macro that's
// already been defined in something like a header guard, then we completely
// skip the file
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
    {string_view{"defined"}, DEFINED},
    {string_view{"#undef"}, UNDEF},
    {string_view{"#pragma"}, PRAGMA}
  }};
  auto constexpr chars_of_interest = string_view{"#/\""};
  // clang-format on
  auto lex = PP_Lexer();
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
        throw Lex_Exc(
            __LINE__,
            std::format("Hash keyword [{}], is not implimented", hash_keyword));
      }

      switch (keyword->second) {
      case INCLUDE: {
        if (i >= file.size)
          throw Lex_Exc(__LINE__, string("Unable to parse include parameter"));

        i = skip_ws(fcontent, i) + 1;

        if (i >= file.size)
          throw Lex_Exc(__LINE__, string("Unable to parse include parameter"));

        lex.types.push_back(INCLUDE);
        switch (fcontent[i]) {
        case '<': {
          lex.types.push_back(LESS);

          end = skip_until<'>'>(fcontent, i + 1);

          if (!(end < file.size))
            throw Lex_Exc(__LINE__, string("Non terminated global include"));

          lex.types.push_back(LIT_STRING);
          lex.lexemes.push_back(string(start + i + 1, start + end));
          i = end + 1;
          lex.types.push_back(GREATER);
        } break;
        case '"': {
          lex.types.push_back(QUOTE);

          end = skip_until<'"'>(fcontent, i + 1);

          if (!(end < file.size))
            throw Lex_Exc(__LINE__, string("Non terminated local include"));

          lex.types.push_back(LIT_STRING);
          lex.lexemes.push_back(string(start + i + 1, start + end));
          i = end + 1;
          lex.types.push_back(QUOTE);
        } break;
        default:
          throw Lex_Exc(__LINE__,
                        std::format("character found = {}", fcontent[i]));
        }
      } break;
      case IFDEF: {
        lex.types.push_back(IFDEF);
        i = skip_ws(fcontent, i) + 1;
        end = skip_until(" \t\n\r", fcontent, i + 1);
        lex.push_lexeme(start + i, start + end);
      } break;
      case IFNDEF: {
        lex.types.push_back(IFNDEF);
        i = skip_ws(fcontent, i) + 1;
        end = skip_until(" \t\n\r", fcontent, i + 1);
        lex.push_lexeme(start + i, start + end);
      } break;
      case DEFINE: {
        lex.types.push_back(DEFINE);
        i = skip_ws(fcontent, i) + 1;
        end = skip_until(" (\t\n\r", fcontent, i + 1);
        lex.push_lexeme(string(start + i, start + end));
        if (fcontent[end] == '(') {
          lex.types.push_back(SPACE);
          lex.types.push_back(LPAREN);
          i = lex.parse_define_args(fcontent, i + 1);
          if (fcontent[i] != ')') {
            throw Lex_Exc(__LINE__,
                          std::format("Expected closing ')' when parsing a "
                                      "function macros arguments"));
          } else {
            lex.types.push_back(RPAREN);
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
      case UNDEF: {
        lex.types.push_back(UNDEF);
        i = skip_ws(fcontent, i) + 1;
        end = skip_until(" \t\n\r", fcontent, i + 1);
        lex.push_lexeme(start + i, start + end);
      } break;
      case IF:
        lex.types.push_back(IF);
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
        throw Lex_Exc(__LINE__, string("'/' found at end of file"));
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
          throw Lex_Exc(__LINE__, string("Non terminated multi line comment"));
      } break;
      default: // probably just an op /
        i = skip_until("#/\"", fcontent, i + 1);
        break;
      }
    } break;
    case '"': {
      i = skip_until<'"'>(fcontent, i + 1) + 1;
      if (!(i < file.size))
        throw Lex_Exc(__LINE__, string("Non terminated string"));
    } break;
    default:
      i = skip_until("#/\"", fcontent, i + 1);
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
      case LESS:
        cur_t += 3; // LANGLE CHAR_LIT RANGLE
        ir.push(IR_AST::IR_Types::GLOBAL_INCLUDE, lexemes[cur_lex]);
        ++cur_lex;
        break;
      case QUOTE:
        cur_t += 3; // QUOTE CHAR_LIT QUOTE
        ir.push(IR_AST::IR_Types::LOCAL_INCLUDE, lexemes[cur_lex]);
        ++cur_lex;
        break;
      default:
        throw Lex_Exc(__LINE__, std::format("Malformed #include statement, "
                                            "expected '<' or '\"', found [{}]",
                                            to_string(types[cur_t])));
      }
      break;
    case IF: {
      ++cur_t;
      auto expr = grab_string(cur_t, cur_lex, ir);
      ir.push(IR_AST::IR_Types::IF, std::move(expr));
    } break;
    case IFDEF:
      cur_t += 2;
      ir.push(IR_AST::IR_Types::IFDEF, lexemes[cur_lex]);
      ++cur_lex;
      break;
    case IFNDEF:
      cur_t += 2;
      ir.push(IR_AST::IR_Types::IFNDEF, lexemes[cur_lex]);
      ++cur_lex;
      break;
    case ENDIF:
      ++cur_t;
      ir.push(IR_AST::IR_Types::ENDIF, "");
      break;
    case ELSE:
      ++cur_t;
      ir.push(IR_AST::IR_Types::ELSE, "");
      break;
    case DEFINE:
      cur_t += 2;
      ir.push(IR_AST::IR_Types::DEFINE, "");
      ++cur_lex;
      break;
    default:
      throw Lex_Exc(__LINE__, std::format("Not implimented, type = [{}]",
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
auto PP_Lexer::handle_hashif(string_view const buf, size_t i) -> size_t {
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
      types.push_back(LPAREN);
      ++i;
    } break;
    case ')': {
      types.push_back(RPAREN);
      ++i;
    } break;
    case '|': {
      ++i;
      if (i < buf.size() && buf[i] == '|') {
        ++i;
        types.push_back(OR);
      } else {
        types.push_back(BIT_OR);
      }
    } break;
    case '&': {
      ++i;
      if (i < buf.size() && buf[i] == '&') {
        ++i;
        types.push_back(AND);
      } else {
        types.push_back(BIT_AND);
      }
    } break;
    case '=': {
      ++i;
      if (i < buf.size() && buf[i] == '=') {
        ++i;
        types.push_back(EQ_EQ);
      } else {
        types.push_back(EQ);
      }
    } break;
    case '!': {
      ++i;
      if (i < buf.size() && buf[i] == '=') {
        ++i;
        types.push_back(BANG_EQ);
      } else {
        types.push_back(BANG);
      }
    } break;
    case '<': {
      ++i;
      if (i < buf.size() && buf[i] == '=') {
        ++i;
        types.push_back(LESS_EQ);
      } else {
        types.push_back(LESS);
      }
    } break;
    case '>': {
      ++i;
      if (i < buf.size() && buf[i] == '=') {
        ++i;
        types.push_back(GREATER_EQ);
      } else {
        types.push_back(GREATER);
      }
    } break;
    case '#': {
      ++i;
      if (i < buf.size() && buf[i] == '#') {
        ++i;
        types.push_back(STRINGIZING);
      } else {
        types.push_back(CONCAT);
      }
    } break;
    case 'd': {
      if (i + defined_str.size() < buf.size() &&
          strncmp(buf.data() + i, defined_str.data(), defined_str.size()) ==
              0) {
        i += defined_str.size();
        types.push_back(DEFINED);
      } else {
        auto const start = i;
        i = skip_until(delims_at(delims::LEXEME), buf, i);
        types.push_back(LEXEME);
        lexemes.push_back(string(buf.data() + start, buf.data() + i));
      }
    } break;
#pragma region octal
    case '0': {
      ++i;
      if (!(i < buf.size())) {
        types.push_back(LIT_INT);
        lexemes.push_back("0");
      } else {
        switch (buf[i]) {
#pragma region binary
        case 'b': {
          // TODO: check if we're on c++14>=, bc otherwise this is supposed to
          // be an error, the same goes with "'" character
          ++i;
          if (!(i < buf.size())) {
            throw Lex_Exc(
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
                throw Lex_Exc(
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
                throw Lex_Exc(
                    __LINE__,
                    std::format("Found [{}], while parsing a binary numbers, "
                                "only 0 and 1 are allowed in binary numbers "
                                "(and ' characters for delimiters)",
                                buf[i]));
              } else if (std::isalpha(buf[i])) {
                throw Lex_Exc(
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
          types.push_back(LIT_BINARY);
          lexemes.push_back(string(buf.data() + start, buf.data() + i));
        } break;
#pragma endregion binary
#pragma region hex
        case 'x': {
          ++i;
          if (!(i < buf.size())) {
            throw Lex_Exc(
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
                throw Lex_Exc(
                    __LINE__,
                    std::format(
                        "When parsing a hex number, found two ' characters "
                        "back to back, these are treated as identifiers for a "
                        "char literal, and thus a formatting error."));
              }
            } else if (is_any_of(delims_at(delims::ALPHA_SANS_HEX), buf.data(),
                                 i)) {
              throw Lex_Exc(__LINE__,
                            std::format("While parsing a hex number, found a "
                                        "character that's not supported."));
            } else {
              inner_looping = false;
            }
          }
          types.push_back(LIT_HEX);
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
                throw Lex_Exc(
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
                throw Lex_Exc(
                    __LINE__,
                    std::format(
                        "While parsing an octal number, encounter [{}], a non "
                        "supposed character in octal numbers",
                        buf[i]));
              }
            }
          }
          types.push_back(LIT_OCTAL);
          lexemes.push_back(string(buf.data() + start, buf.data() + i));
        } break;
        case '8':
          [[fallthrough]];
        case '9': {
          throw Lex_Exc(
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
            throw Lex_Exc(
                __LINE__,
                std::format("While parsing a decimal number, found a double '. "
                            "These are treated as introducing a char literal, "
                            "causing an error."));
          }
          // These two cases should probably be rolled into one
        } else if (is_any_of(delims_at(delims::HEX_SANS_DIGITS), buf.data(),
                             i)) {
          throw Lex_Exc(
              __LINE__,
              std::format("While parsing a decimal number, encountered the "
                          "following unsupported character [{}]",
                          buf[i]));
        } else {
          if (is_any_of(delims_at(delims::LEXEME), buf.data(), i)) {
            break;
          } else {
            throw Lex_Exc(
                __LINE__,
                std::format("While parsing a decimal number, encountered the "
                            "following unsupported character [{}]",
                            buf[i]));
          }
        }
      }
      types.push_back(LIT_INT);
      lexemes.push_back(string(buf.data() + start, buf.data() + i));
    } break;
#pragma endregion decimal
    default: {
      auto const start = i;
      i = skip_until(delims_at(delims::LEXEME), buf, i);
      types.push_back(LEXEME);
      lexemes.push_back(string(buf.data() + start, buf.data() + i));
    } break;
    }
  }
  return i;
}

auto PP_Lexer::parse_define_args(string_view const fcontent, size_t i)
    -> size_t {
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

auto PP_Lexer::grab_string(size_t &cur_t, size_t &cur_lex, IR_AST &ir)
    -> string {
  auto constexpr is_terminator = [](enum PP_Lexer::types const t) -> bool {
    auto constexpr terminator_list = std::array<enum PP_Lexer::types, 11>{
        {PP_Lexer::IF, PP_Lexer::IFDEF, PP_Lexer::IFNDEF, PP_Lexer::ELIF,
         PP_Lexer::ELSE, PP_Lexer::ENDIF, PP_Lexer::DEFINE,
         PP_Lexer::SPACE, // only used when dealing with #define directives
         PP_Lexer::INCLUDE, PP_Lexer::UNDEF, PP_Lexer::PRAGMA}};
    return std::any_of(terminator_list.begin(), terminator_list.end(),
                       [t](auto &&x) { return x == t; });
  };
  auto str = string();
  while (!is_terminator(types[cur_t])) {
    switch (types[cur_t]) {
    case PP_Lexer::AND:
      str += "&&";
      ++cur_t;
      break;
    case PP_Lexer::OR:
      str += "||";
      ++cur_t;
      break;
    case PP_Lexer::BIT_AND:
      str += "&";
      ++cur_t;
      break;
    case PP_Lexer::BIT_OR:
      str += "|";
      ++cur_t;
      break;
    case PP_Lexer::DEFINED:
      str += "defined";
      ++cur_t;
      break;
    case PP_Lexer::LESS:
      str += "<";
      ++cur_t;
      break;
    case PP_Lexer::LESS_EQ:
      str += "<=";
      ++cur_t;
      break;
    case PP_Lexer::GREATER:
      str += ">";
      ++cur_t;
      break;
    case PP_Lexer::GREATER_EQ:
      str += ">=";
      ++cur_t;
      break;
    case PP_Lexer::STRINGIZING:
      str += "#";
      ++cur_t;
      break;
    case PP_Lexer::CONCAT:
      str += "##";
      ++cur_t;
      break;
    case PP_Lexer::PLUS:
      str += "+";
      ++cur_t;
      break;
    case PP_Lexer::MINUS:
      str += "-";
      ++cur_t;
      break;
    case PP_Lexer::STAR:
      str += "*";
      ++cur_t;
      break;
    case PP_Lexer::SLASH:
      str += "/";
      ++cur_t;
      break;
    case PP_Lexer::BANG_EQ:
      str += "!=";
      ++cur_t;
      break;
    case PP_Lexer::EQ:
      str += "=";
      ++cur_t;
      break;
    case PP_Lexer::EQ_EQ:
      str += "==";
      ++cur_t;
      break;
    case PP_Lexer::BANG:
      str += "!";
      ++cur_t;
      break;
    case PP_Lexer::LPAREN:
      str += "(";
      ++cur_t;
      break;
    case PP_Lexer::RPAREN:
      str += ")";
      ++cur_t;
      break;
    case PP_Lexer::QUOTE:
      str += "\"";
      ++cur_t;
      break;
    case PP_Lexer::MACRO:
      [[fallthrough]];
    case PP_Lexer::LIT_CHAR:
      [[fallthrough]];
    case PP_Lexer::LIT_STRING:
      [[fallthrough]];
    case PP_Lexer::LIT_INT:
      [[fallthrough]];
    case PP_Lexer::LIT_HEX:
      [[fallthrough]];
    case PP_Lexer::LIT_OCTAL:
      [[fallthrough]];
    case PP_Lexer::LIT_BINARY:
      [[fallthrough]];
    case PP_Lexer::LIT_FLOAT:
      [[fallthrough]];
    case PP_Lexer::LEXEME:
      str += lexemes[cur_lex++];
      ++cur_t;
      break;
    default:
      break;
    }
  }
  return str;
}

auto PP_Lexer::produce_string(string_view const fcontent, size_t &i) -> string {
  throw std::runtime_error("produce_string Not impl");
}

/*
auto PP_Lexer::parse_expr(size_t &cur_t, size_t &cur_lex, IR_AST &ir)
    -> Expr_Node {

  return equality(cur_t, cur_lex, ir);
}
*/

#if 0
auto PP_Lexer::equality(size_t &cur_t, size_t &cur_lex, IR_AST &ir)
    -> Expr_Node {
  auto lhs = comparison(cur_t, cur_lex, ir);

  while (matching(cur_t, {PP_Lexer::BANG_EQ, PP_Lexer::EQ_EQ})) {
    auto const tkn_t = types[cur_t++];
    auto rhs = comparison(cur_t, cur_lex, ir);
    lhs = ir.make_binary(tkn_t, lhs, rhs);
  }

  return lhs;
}

auto PP_Lexer::comparison(size_t &cur_t, size_t &cur_lex, IR_AST &ir)
    -> Expr_Node {
  auto lhs = term(cur_t, cur_lex, ir);

  while (matching(cur_t, {PP_Lexer::GREATER, PP_Lexer::GREATER_EQ,
                          PP_Lexer::LESS, PP_Lexer::LESS_EQ})) {
    auto const tkn_t = types[cur_t++];
    auto rhs = term(cur_t, cur_lex, ir);
    lhs = ir.make_binary(tkn_t, lhs, rhs);
  }

  return lhs;
}

auto PP_Lexer::term(size_t &cur_t, size_t &cur_lex, IR_AST &ir) -> Expr_Node {
  auto lhs = factor(cur_t, cur_lex, ir);

  while (matching(cur_t, {PP_Lexer::MINUS, PP_Lexer::PLUS})) {
    auto const tkn_t = types[cur_t++];
    auto rhs = factor(cur_t, cur_lex, ir);
    lhs = ir.make_binary(tkn_t, lhs, rhs);
  }

  return lhs;
}

auto PP_Lexer::factor(size_t &cur_t, size_t &cur_lex, IR_AST &ir) -> Expr_Node {
  auto lhs = unary(cur_t, cur_lex, ir);

  while (matching(cur_t, {PP_Lexer::SLASH, PP_Lexer::STAR})) {
    auto const tkn_t = types[cur_t++];
    auto rhs = unary(cur_t, cur_lex, ir);
    lhs = ir.make_binary(tkn_t, lhs, rhs);
  }

  return lhs;
}

auto PP_Lexer::unary(size_t &cur_t, size_t &cur_lex, IR_AST &ir) -> Expr_Node {
  if (matching(cur_t, {PP_Lexer::BANG, PP_Lexer::MINUS})) {
    auto const tkn_t = types[cur_t++];
    auto un = unary(cur_t, cur_lex, ir);
    return ir.make_unary(tkn_t, un);
  } else {
    return primary(cur_t, cur_lex, ir);
  }
}

auto PP_Lexer::primary(size_t &cur_t, size_t &cur_lex, IR_AST &ir)
    -> Expr_Node {
  switch (types[cur_t]) {
  case LIT_CHAR:
    return ir.make_charlit(LIT_CHAR, lexemes[cur_lex++]);
  case LIT_STRING:
    return ir.make_primary(LIT_STRING, lexemes[cur_lex++]);
  case LIT_INT:
    return ir.make_primary(LIT_INT, lexemes[cur_lex++]);
  case LIT_HEX:
    return ir.make_primary(LIT_HEX, lexemes[cur_lex++]);
  case LIT_OCTAL:
    return ir.make_primary(LIT_OCTAL, lexemes[cur_lex++]);
  case LIT_BINARY:
    return ir.make_primary(LIT_BINARY, lexemes[cur_lex++]);
  case LIT_FLOAT:
    return ir.make_primary(LIT_FLOAT, lexemes[cur_lex++]);
  case LEXEME:
    return ir.make_primary(LEXEME, lexemes[cur_lex++]);
  case LPAREN: {
    ++cur_t;
    auto const expr = parse_expr(cur_t, cur_lex, ir);
    expect(cur_t, PP_Lexer::RPAREN);
    return ir.grouping(expr);
  }
  default:
    throw std::runtime_error("Unsupported token in expression.");
  }
}
#endif

auto PP_Lexer::expect(size_t cur_t, enum PP_Lexer::types tkn) -> void {
  if (types[cur_t] != tkn) {
    throw Parse_Exc(__LINE__,
                    std::format("Unexpected token, expected {}, found {}",
                                to_string(tkn), to_string(types[cur_t])));
  }
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

auto Expr_Node::eval() const -> int {
  throw std::runtime_error("Expr_Node::eval not impl");
}

IR_AST::IR_AST()
    : size(0), cap(8), ast(std::make_unique<IR_Types[]>(cap)),
      exprs(std::make_unique<StringViews[]>(cap)) {}

auto parse(FixedString const &file) -> IR_AST {
#ifdef DEBUG
  auto lexer = PP_Lexer::lex(file);
  std::cout << "lexer\n";
  lexer.display(std::cout);
  std::cout.flush();
  return lexer.parse_to_ast();
#else
  return PP_Lexer::lex(file).parse_to_ast();
#endif
}

auto IR_AST::check_size() -> void {
  if (size == cap) {
    auto const new_cap = cap * 2;
    auto new_ast = std::make_unique<IR_AST::IR_Types[]>(new_cap);
    auto new_exprs = std::make_unique<StringViews[]>(new_cap);

    std::memmove(new_ast.get(), ast.get(), sizeof(enum IR_AST::IR_Types) * cap);
    std::memmove(new_exprs.get(), exprs.get(), sizeof(StringViews) * cap);
    ast = std::move(new_ast);
    exprs = std::move(new_exprs);
    cap = new_cap;
  }
}

auto IR_AST::push(IR_AST::IR_Types t, string const &expr) -> void {
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

auto IR_AST::make_charlit(std::string_view const sv) -> Expr_Node {
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

auto IR_AST::make_nonelit() const -> Expr_Node { return Expr_Node(); }

#ifdef DEBUG
auto IR_AST::shitty_display(std::ostream &out) const -> std::ostream & {
  out << "lexemes = [" << string_view{lexemes.buffer, lexemes.size} << "]\n";
  out << "ast:\n\t";
  for (auto i = size_t{}; i < size; ++i) {
    out << '[' << IR_AST::pretty_types(ast[i]) << ']';
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

auto IR_Interpreter::interpret(IR_AST const &ir) -> vector<fs::path> {
  auto vec = vector<fs::path>();
  vec.reserve(ir.num_includes());
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
    auto const lit = ir.exprs[i];

    auto const include_name = string(string_view{ir.lexemes.buffer + lit.start,
                                                 ir.lexemes.buffer + lit.end});
    vec.push_back(include_name);
    ++i;
  } break;
  case IR_AST::IR_Types::IF: {
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
  case IR_AST::IR_Types::IFDEF: {
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
        throw Interpret_Exc(__LINE__, "Unterminated #ifdef expression");
      i = skip_until(i, std::span(ir.ast.get(), ir.size),
                     IR_AST::IR_Types::ENDIF);
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

      i = skip_until(i, std::span(ir.ast.get(), ir.size),
                     IR_AST::IR_Types::ENDIF);
      if (i == ir.size) {
        throw Interpret_Exc(__LINE__, "Unterminated #ifdef expression");
      }
      ++i; // move over the #endif
    }
  } break;
  case IR_AST::IR_Types::IFNDEF: {
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
        throw Interpret_Exc(__LINE__, "Unterminated #ifndef expression");
      i = skip_until(i, std::span(ir.ast.get(), ir.size),
                     IR_AST::IR_Types::ENDIF);
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

      i = skip_until(i, std::span(ir.ast.get(), ir.size),
                     IR_AST::IR_Types::ENDIF);
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
    auto const lit = ir.exprs[i];
    auto const defining_macro = string(string_view{
        ir.lexemes.buffer + lit.start, ir.lexemes.buffer + lit.end});
    ++i;
    def_macros.emplace(defining_macro);
  } break;
  default:
    throw Interpret_Exc(__LINE__, std::format("Not implimented, type = [{}]",
                                              IR_AST::pretty_types(ir.ast[i])));
  }
}

auto IR_Interpreter::eval(IR_AST const &ir, size_t &i) const -> int {
  auto const expr = expand_expr(ir, i);
  return expr.eval();
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

// TODO: the way we do this is by splitting the string around the lexemes, then
// combining them together, it results in a lot of allocations and
// deallocations, there is 100% a better way of doing this, like with just a
// buffer, and some pointers showing where we have space
// [[https://www.youtube.com/watch?v=g2hiVp6oPZc]] this video talks about a neat
// data structure that should be able to solve the problem, but i'm just trying
// to get this to work :)
// TODO: handle function macros
auto IR_Interpreter::expand_expr(IR_AST const &ir, size_t &i) const
    -> Expr_Node {
  fn_print();
  auto &&[start, end] = ir.exprs[i];
  auto str = string(ir.lexemes.buffer + start, ir.lexemes.buffer + end);
#ifdef DEBUG
  std::cout << "trying to expand [" << str << "]\n";
  std::cout.flush();
#endif // DEBUG
  auto expanding = true;
  while (expanding) {
#ifdef DEBUG
    std::cout << "expanding [" << str << "]\n";
    std::cout.flush();
#endif // DEBUG
    if (auto &&[found, poses] = contains_lexemes(str); found) {
      auto const head = str.substr(0, poses.first);
      auto const lex = str.substr(poses.first, poses.second);
#ifdef DEBUG
      std::cout << "lex looking for = [" << lex << "]\n";
      std::cout.flush();
#endif // DEBUG
      // TODO: emit a warning when the lexeme is not found, it's well defined by
      // the standard, but it's really weird
      auto const val = macros.find(lex) != macros.end()
                           ? macros.find(lex)->second
                       : def_macros.find(lex) != def_macros.end() ? string("")
                                                                  : string("0");
      auto const tail = str.substr(poses.second, str.npos);
      str = std::format("{}{}{}", head, val, tail);
    } else {
      // done expanding, now we have to parse this into an ast
      expanding = false;
    }
  }
#ifdef DEBUG
  std::cout << "fully macro expanded expr = [" << str << "]\n";
  std::cout.flush();
#endif // DEBUG
  throw std::runtime_error("expand_expr not impl");
}
} // namespace ir
} // namespace luamake
