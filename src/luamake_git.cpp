#include "luamake_git.hpp"
#include <array>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace luamake::builtins {
namespace {
auto constexpr git_lib = std::array<luaL_Reg, 1>{luaL_Reg{nullptr, nullptr}};
auto luaopen_git(lua_State *state) -> int {
  luaL_newlib(state, git_lib);
  return 1;
}
} // namespace

// maybe turn this into a luac function, that way we can call it with pcall, in
// case something goes wrong
auto open_git(lua_State *state) -> bool {
  luaL_requiref(state, "git", luaopen_git, true);
  lua_pop(state, 1); // remove git from top of the stack
  return false;
}
} // namespace luamake::builtins
