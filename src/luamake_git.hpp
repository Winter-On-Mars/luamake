#ifndef __LUAMAKE_GIT_HPP
#define __LUAMAKE_GIT_HPP

extern "C" {
#include "lua.h"
}

namespace luamake::builtins {
auto open_git(lua_State *) -> bool;
}

#endif // !__LUAMAKE_GIT_HPP
