#include "luamake_builtins.hpp"

#include "common.hpp"
#include "lua/lua.h"

#include <array>

#include <iostream>

namespace luamake_builtins {
auto clang(lua_State *state) -> int {
  fn_print();

  auto num_args = lua_gettop(state);
  if (num_args != 1) {
    lua_pushnil(state);
    return 1;
  }
  auto constexpr compiler_field = std::string_view{"clang++"};
  auto constexpr opt_level = std::string_view{"O2"};
  auto constexpr warnings = std::array<std::string_view, 3>{{
      std::string_view{"Wall"},
      std::string_view{"Wconversion"},
      std::string_view{"Wpedantic"},
  }};

  lua_createtable(state, 0, 3); // tbl

  lua_pushstring(state, compiler_field.data());
  lua_setfield(state, -2,
               "compiler"); // setfield pops the value from the stack :)

  lua_pushstring(state, opt_level.data());
  lua_setfield(state, -2, "optimize");

  lua_createtable(state, 3, 0);
  auto constexpr table_idx = int{-2};
  for (auto idx = lua_Integer{1}; auto const warning : warnings) {
    lua_pushstring(state, warning.data());
    lua_seti(state, table_idx, idx); // pops the val from the stack :)
    ++idx;
  }

  lua_setfield(state, -2, "warnings");

  exit_fn_print();

  return 1;
}

// TODO: finish function
static auto install_exe(lua_State *state) -> int {
  fn_print();
  using std::string;

  // stack: [self] [exe]
  auto num_args = lua_gettop(state);
  if (num_args != 2) {
    lua_pushstring(state, "Too many arguments.");
    return lua_error(state);
  }

  lua_getfield(state, -1, "name");
  auto name = string(lua_tolstring(state, -1, nullptr));
  lua_pop(state, 1);
  lua_getfield(state, -1, "root");
  auto root_file = string(lua_tolstring(state, -1, nullptr));
  lua_pop(state, 1);

  std::cerr << "name = " << name << ",\nroot = " << root_file << "\n";

  lua_pushnil(state);
  exit_fn_print();
  return 1;
}

auto make_builder_obj(lua_State *state, std::string_view const builder_obj)
    -> void {
  fn_print();
  lua_createtable(state, 0, 2);

  lua_pushstring(state, ".");
  lua_setfield(state, -2, "install_dir");

  lua_pushcfunction(state, install_exe);
  lua_setfield(state, -2, "install_exe");

  // TODO: add the functions install_dynamic and install_static
  exit_fn_print();
}
} // namespace luamake_builtins
