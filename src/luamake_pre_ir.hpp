#ifndef __LUAMAKE_PRE_IR_HPP
#define __LUAMAKE_PRE_IR_HPP

// NOTE: this is great, and i love this, but we should probably think about just
// having a dependency on treesitter, because it should be able to handle all
// that we would need in terms of reading the headers, and it's supposed to be
// fast, so hopefully it will be, something worth trying

#include "luamake_allocator.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef DEBUG_CPP
#include <ostream>
#endif // DEBUG_CPP

#ifndef LM_EXPR_ALLOC_SIZE
#define LM_EXPR_ALLOC_SIZE 2 << 8
#endif // !LM_EXPR_ALLOC_SIZE

namespace luamake {
namespace pp {
using Macro = std::string;

// [[https://stackoverflow.com/questions/34596768/stdunordered-mapfind-using-a-type-different-than-the-key-type/53530846#53530846]]
struct StringHasher final {
  using hash_type = std::hash<std::string_view>;
  using is_transparent = void;
  inline auto operator()(std::string_view const sv) const -> size_t {
    return hash_type{}(sv);
  }
  inline auto operator()(std::string const &s) const -> size_t {
    return hash_type{}(s);
  }
};

using StringMap =
    std::unordered_map<std::string, Macro, StringHasher, std::equal_to<>>;
using StringSet =
    std::unordered_set<std::string, StringHasher, std::equal_to<>>;

struct Interpreter final {
  Interpreter(StringMap &&macros, StringSet &&def_macros) noexcept
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
  auto interpret(std::string_view const, allocator::Page<LM_EXPR_ALLOC_SIZE> &)
      -> std::vector<std::filesystem::path>;

#ifdef DEBUG_CPP
  auto dump_macros(std::ostream &) noexcept -> void;
#endif // DEBUG_CPP

private:
  StringMap macros;
  StringSet def_macros;
};
} // namespace pp
} // namespace luamake

#endif
