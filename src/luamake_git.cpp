#include "luamake_git.hpp"

#include <array>
#include <iostream>

extern "C" {
#include "lua/lauxlib.h"
#include "lua/lua.h"
}

namespace luamake::builtins {
namespace {
auto clone(lua_State *clone) -> int {
  std::cout << "Hello from git.clone\n";
  return 0;
}

auto constexpr git_lib = std::array<luaL_Reg, 2>{luaL_Reg{"clone", &clone},
                                                 luaL_Reg{nullptr, nullptr}};
} // namespace

auto luaopen_git(lua_State *state) -> int {
  luaL_checkversion(state);
  lua_createtable(state, 0, git_lib.size() - 1);
  luaL_setfuncs(state, git_lib.data(), 0);
  return 1;
}
} // namespace luamake::builtins
