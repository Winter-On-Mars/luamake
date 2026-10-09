#ifndef __LUAMAKE_SPIRAL_HPP
#define __LUAMAKE_SPIRAL_HPP

#include <filesystem>
#include <variant>
#include <vector>

namespace luamake {
namespace builtins {
struct Module;
}

namespace spl {
auto serialize(builtins::Module const &, std::vector<std::string> &&,
               std::filesystem::path const &) noexcept(false) -> void;

auto deserialize(std::filesystem::path const &path) noexcept(false)
    -> std::variant<std::pair<builtins::Module, std::vector<std::string>>,
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
