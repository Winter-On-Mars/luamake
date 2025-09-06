#include "luamake_pre_ir.hpp"
#include "luamake_strings.hpp"

#include <cstring>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
namespace fs = std::filesystem;

using std::string, std::string_view, std::vector, std::unordered_map,
    std::unordered_set;

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
// size != strlen(delims)
auto constexpr skip_until(char const *delims, size_t const size,
                          char const *buffer, size_t i) -> size_t {
  auto const str_len = string_len(delims);
  while (i < size) {
    for (auto idx = size_t{}; idx < str_len; ++idx) {
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
    }
  }
  return size;
}
} // namespace

// i would like to add lexical short cutting, where if we see a macro that's
// already been defined in something like a header guard, then we completely
// skip the file
// TODO: there's actually a lot more we need to do for macro preprocessing, like
// properly parsing expressions (adding them to the lexer :))
auto IR::Lexer::lex(FixedString const &file) -> Lexer {
  // clang-format off
  static auto const keywords = unordered_map<string_view, enum Lexer::types>{{
    {string_view{"#if"}, IF},
    {string_view{"#ifdef"}, IFDEF},
    {string_view{"#ifndef"}, IFNDEF},
    {string_view{"#elif"}, ELIF},
    {string_view{"#else"}, ELSE},
    {string_view{"#endif"}, ENDIF},
    {string_view{"#define"}, DEFINE},
    {string_view{"#include"}, INCLUDE},
    {string_view{"defined"}, OP_DEFINED},
  }};
  // clang-format on
  auto lex = Lexer();

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
        i = skip_until("#/\"", file.size, fcontent, i);
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

          lex.types.emplace_back(CHAR_LIT);
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

          lex.types.emplace_back(CHAR_LIT);
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
      case OP_DEFINED: {
        // TODO
        throw Lex_Exc(__LINE__, string("Not implimented"));
      } break;
      case IFDEF: {
        lex.types.push_back(IFDEF);
        i = skip_ws(file.size, fcontent, i) + 1;
        end = skip_until(" \t\n\r", file.size, fcontent, i + 1);
        lex.types.push_back(CHAR_LIT);
        lex.lexemes.emplace_back(string_view{fcontent + i, fcontent + end});
      } break;
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

auto IR::Lexer::parse_to_ir() -> IR {
  auto ir = IR();
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
        ir.push(IR::GLOBAL_INCLUDE, lexemes[cur_lex++]);
        break;
      case QUOTE:
        cur_t += 3; // QUOTE CHAR_LIT QUOTE
        ir.push(IR::LOCAL_INCLUDE, lexemes[cur_lex++]);
        break;
      default:
        throw Lex_Exc(__LINE__, std::format("Malformed #include statement, "
                                            "expected '<' or '\"', found [{}]",
                                            to_string(types[cur_t])));
      }
      break;
    case IFDEF:
      cur_t += 2;
      ir.push(IR::IFDEF, lexemes[cur_lex++]);
      break;
    case IFNDEF:
      cur_t += 2;
      ir.push(IR::IFNDEF, lexemes[cur_lex++]);
      break;
    case ENDIF:
      ++cur_t;
      ir.push(IR::ENDIF, "");
      break;
    case ELSE:
      ++cur_t;
      ir.push(IR::ELSE, "");
      break;
    default:
      throw Lex_Exc(__LINE__, std::format("Not implimented, type = [{}]",
                                          to_string(types[cur_t])));
      break;
    }
  }
  return ir;
}

auto IR::Lexer::to_string(enum types t) const -> string {
  switch (t) {
  case IF:
    return string("IF");
  case IFDEF:
    return string("IFDEF");
  case IFNDEF:
    return string("IFNDEF");
  case ELIF:
    return string("ELIF");
  case ELSE:
    return string("ELSE");
  case ENDIF:
    return string("ENDIF");
  case DEFINE:
    return string("DEFINE");
  case INCLUDE:
    return string("INCLUDE");
  case LOG_AND:
    return string("LOG_AND");
  case LOG_OR:
    return string("LOG_OR");
  case OP_DEFINED:
    return string("OP_DEFINED");
  case LPAREN:
    return string("LPAREN");
  case RPAREN:
    return string("RPAREN");
  case LANGLE:
    return string("LANGLE");
  case RANGLE:
    return string("RANGLE");
  case QUOTE:
    return string("QUOTE");
  case CHAR_LIT:
    return string("CHAR_LIT");
  case INT_LIT:
    return string("INT_LIT");
  case MACRO:
    return string("MACRO");
  }
}

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

IR::IR()
    : size(0), cap(8), types(std::make_unique<enum types[]>(cap)),
      exprs(std::make_unique<std::string[]>(cap)) {}

auto IR::parse(FixedString const &file) -> IR {
  return Lexer::lex(file).parse_to_ir();
}

auto IR::interpret(unordered_map<string, Macro> &macros,
                   unordered_set<string> &def_macros) -> vector<fs::path> {
  auto vec = vector<fs::path>();
  for (auto i = size_t{}; i < size;) {
    interpret_impl(i, false, macros, def_macros, vec);
  }
  return vec;
}

auto IR::interpret_impl(size_t &i, bool interpret_elses,
                        std::unordered_map<std::string, Macro> &macros,
                        std::unordered_set<std::string> &def_macros,
                        vector<fs::path> &vec) -> void {
  switch (types[i]) {
  case GLOBAL_INCLUDE: {
    // TODO: check that this file *actually exists*
    ++i;
  } break;
  case LOCAL_INCLUDE: {
    vec.push_back(exprs[i++]);
  } break;
  case IFDEF: {
    auto const checking_macro = exprs[i];
    auto const defined =
        macros.contains(checking_macro) || def_macros.contains(checking_macro);
    ++i;
    if (defined) {
      while (i < size) {
        if (types[i] == ELSE || types[i] == ENDIF || types[i] == ELIF)
          break;
        interpret_impl(i, false, macros, def_macros, vec);
      }
      if (i == size)
        throw Interpret_Exc(__LINE__, "Unterminated #ifdef expression");
      while (i < size && types[i] != ENDIF) {
        ++i;
      }
      if (i == size)
        throw Interpret_Exc(__LINE__, "Unterminated #ifdef expression");
      ++i; // move over the ENDIF
    } else {
      while (i < size) {
        if (!(types[i] == ELSE || types[i] == ENDIF || types[i] == ELIF))
          ++i;
      }
      if (i == size)
        throw Interpret_Exc(__LINE__, "Unterminated #ifdef expression");

      interpret_impl(i, true, macros, def_macros, vec);

      while (i < size && types[i] != ENDIF) {
        ++i;
      }
      if (i == size) {
        throw Interpret_Exc(__LINE__, "Unterminated #ifdef expression");
      }
    }
  } break;
  default:
    throw Interpret_Exc(__LINE__, std::format("Not implimented, type = [{}]",
                                              pretty_types(types[i])));
  }
}

auto IR::check_size() -> void {
  if (size == cap) {
    auto const new_cap = cap * 2;
    auto new_types = std::make_unique<enum IR::types[]>(new_cap);
    auto new_exprs = std::make_unique<std::string[]>(new_cap);

    std::memmove(new_types.get(), types.get(), sizeof(enum IR::types) * cap);
    for (auto i = size_t{}; i < cap; ++i) {
      new_exprs[i] = std::move(exprs[i]);
    }
    types = std::move(new_types);
    exprs = std::move(new_exprs);
    cap = new_cap;
  }
}

auto IR::push(enum types &&t, string_view &&sv) -> void {
  types[size] = t;
  exprs[size++] = sv;
}
} // namespace ir
} // namespace luamake
