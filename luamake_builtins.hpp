#ifndef __LUAMAKE_BUILTINS_HPP
#define __LUAMAKE_BUILTINS_HPP

#include <string_view>

extern "C" {
#include "lua/lua.h"
}

namespace luamake {
namespace builtins {
auto dump(lua_State *state) noexcept -> int;

auto clang(lua_State *state) noexcept -> int;

auto make_builder_obj(lua_State *state,
                      std::string_view const builder_obj) noexcept -> void;

auto make_runner_obj(lua_State *state,
                     std::string_view const runner_obj) noexcept -> void;
} // namespace builtins
} // namespace luamake

#endif
