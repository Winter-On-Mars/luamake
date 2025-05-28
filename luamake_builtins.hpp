#ifndef __LUAMAKE_BUILTINS_HPP
#define __LUAMAKE_BUILTINS_HPP

#include <string_view>

extern "C" {
#include "lua/lua.h"
}

namespace luamake_builtins {
auto clang(lua_State *state) -> int;

auto make_builder_obj(lua_State *state, std::string_view const builder_obj)
    -> void;
} // namespace luamake_builtins

#endif
