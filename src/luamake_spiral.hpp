#ifndef __LUAMAKE_SPIRAL_HPP
#define __LUAMAKE_SPIRAL_HPP

#include <filesystem>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace luamake {
// forward decls because i'm lazy
namespace builtins {
struct Module;
}

namespace spl {
auto serialize(builtins::Module const &, std::span<std::string_view const> &&,
               std::filesystem::path const &) noexcept(false) -> void;

auto deserialize(std::filesystem::path const &path) noexcept(false)
    -> std::variant<std::pair<builtins::Module, std::vector<std::string_view>>,
                    std::string>;

template <class T> struct Serializer {
  Serializer() = delete;
  auto serialize(T) -> void = delete;
  auto serialize(T const &) -> void = delete;
  auto serialize(T &&) -> void = delete;
};

template <class T> struct Deserializer {
  Deserializer() = delete;
  auto deserialize() -> T = delete;
};
} // namespace spl
} // namespace luamake

#endif // !__LUAMAKE_SPIRAL_HPP
