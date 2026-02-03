#ifndef __LUAMAKE_PRE_IR_HPP
#define __LUAMAKE_PRE_IR_HPP

#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace luamake {
namespace pp {
struct Exception {
  explicit Exception(std::string &&message) noexcept : message(message) {}

  ~Exception() = default;
  auto what() const noexcept -> std::string;

  std::string message;
};

using Macro = std::string;

struct Interpreter final {
  Interpreter(std::unordered_map<std::string, Macro> &&macros,
              std::unordered_set<std::string> &&def_macros) noexcept
      : macros(macros), def_macros(def_macros) {}

  Interpreter() noexcept = default;
  Interpreter(Interpreter &&) noexcept = default;
  Interpreter &operator=(Interpreter &&) noexcept = default;
  ~Interpreter() noexcept = default;

  Interpreter(Interpreter const &) noexcept = delete;
  Interpreter &operator=(Interpreter const &) noexcept = delete;
  /**
   * @throws Exception
   */
  [[nodiscard]]
  auto interpret(std::string_view const) -> std::vector<std::filesystem::path>;

private:
  std::unordered_map<std::string, Macro> macros;
  std::unordered_set<std::string> def_macros;
};
} // namespace pp
} // namespace luamake

#endif
