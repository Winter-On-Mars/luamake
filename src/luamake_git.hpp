#ifndef __LUAMAKE_GIT_HPP
#define __LUAMAKE_GIT_HPP

extern "C" {
#include "lua/lua.h"
}

namespace luamake::builtins {
auto luaopen_git(lua_State *) -> int;
}

#endif // !__LUAMAKE_GIT_HPP
