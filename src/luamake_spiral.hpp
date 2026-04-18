#ifndef __LUAMAKE_SPIRAL_HPP
#define __LUAMAKE_SPIRAL_HPP

#include <filesystem>
#include <variant>

namespace luamake {
namespace builtins {
struct Module;
}

// TODO: this module is broken, idk how we fix it :)
namespace spl {
auto serialize(builtins::Module const &, std::filesystem::path const &) -> void;

auto deserialize(std::filesystem::path const &path)
    -> std::variant<builtins::Module, std::string>;

struct Serializer;
struct Deserializer;
} // namespace spl
} // namespace luamake

#endif // !__LUAMAKE_SPIRAL_HPP
