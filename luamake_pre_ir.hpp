#ifndef __LUAMAKE_PRE_IR_HPP
#define __LUAMAKE_PRE_IR_HPP

#include "luamake_strings.hpp"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace luamake {
namespace pp {
struct Exception {
  explicit Exception(decltype(__LINE__) line, std::string &&message) noexcept
      : line(line), message(message) {}

  ~Exception() = default;
  auto what() const noexcept -> std::string;

  decltype(__LINE__) line;
  std::string message;
};

using Macro = std::string;

struct Ast;

struct Interpreter final {
  constexpr Interpreter(std::unordered_map<std::string, Macro> &macros,
                        std::unordered_set<std::string> &def_macros)
      : macros(macros), def_macros(def_macros) {}

  /**
   * @throws Interpret_Exc <: Exception
   */
  [[nodiscard]]
  auto interpret(FixedString const &) -> std::vector<std::filesystem::path>;

private:
  std::unordered_map<std::string, Macro> &macros;
  std::unordered_set<std::string> &def_macros;
};
} // namespace pp
} // namespace luamake

#endif
