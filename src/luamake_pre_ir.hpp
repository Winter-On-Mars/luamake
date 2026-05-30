#ifndef __LUAMAKE_PRE_IR_HPP
#define __LUAMAKE_PRE_IR_HPP

// NOTE: this is great, and i love this, but we should probably think about just
// having a dependency on treesitter, because it should be able to handle all
// that we would need in terms of reading the headers, and it's supposed to be
// fast, so hopefully it will be, something worth trying

#include <filesystem>
#include <ostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace luamake {
namespace pp {
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
   * @throws std::runtime_error
   */
  [[nodiscard]]
  auto interpret(std::string_view const) -> std::vector<std::filesystem::path>;

#ifdef DEBUG
  auto dump_macros(std::ostream &) noexcept -> void;

#endif // DEBUG

private:
  std::unordered_map<std::string, Macro> macros;
  std::unordered_set<std::string> def_macros;
};
} // namespace pp
} // namespace luamake

#endif
