#include "luamake_builtins.hpp"

#include "common.hpp"
#include <filesystem>
#include <system_error>

extern "C" {
#include "lua/lua.h"
}

#include <array>
#include <cstdlib>
#include <format>
#include <iostream>
#include <string>
#include <string_view>

namespace luamake_builtins {
using std::array, std::string, std::string_view;

static auto parse_compiler_table(lua_State *) -> string;

struct Module final {
  enum Module_t {
    EXE,
    STATIC,
    DYNAMIC,
  };
  Module_t type;

  char const *name;
  fs::path root;
  std::string compiler;
  char const *install_dir;
  // other module deps

  static auto make(Module_t type, lua_State *state) noexcept -> Module {
    auto ret_t = Module{};
    ret_t.type = type;

    lua_getfield(state, -1, "name");
    ret_t.name = lua_tolstring(state, -1, nullptr);

    lua_getfield(state, -2, "root");
    ret_t.root = lua_tolstring(state, -1, nullptr);

    lua_getfield(state, -3, "compiler");
    ret_t.compiler = parse_compiler_table(state);

    lua_getfield(state, -4, "install_dir");
    ret_t.install_dir = lua_tolstring(state, -1, nullptr);

    lua_pop(state, 4); // might cause an issue? just trying to restore the stack

    return ret_t;
  }
};

auto clang(lua_State *state) -> int {
  fn_print();

  auto num_args = lua_gettop(state);
  if (num_args != 1) {
    lua_pushstring(state, "Too many arguments");
    return lua_error(state);
  }
  auto constexpr compiler_field = string_view{"clang++"};
  auto constexpr opt_level = string_view{"O2"};
  auto constexpr warnings = array<string_view, 3>{{
      string_view{"Wall"},
      string_view{"Wconversion"},
      string_view{"Wpedantic"},
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

static auto parse_compiler_table(lua_State *state) -> string {
  auto str = string();

  lua_getfield(state, -1, "compiler");
  str += lua_tolstring(state, -1, nullptr);

  lua_getfield(state, -2, "optimize");
  str += " -";
  str += lua_tolstring(state, -1, nullptr);

  lua_getfield(state, -3, "warnings");
  auto tbl_idx = -1;
  auto num_warnings = lua_rawlen(state, tbl_idx);
  for (auto i = lua_Unsigned{1}; i <= num_warnings; ++i) {
    switch (lua_geti(state, tbl_idx, static_cast<lua_Integer>(i))) {
    case LUA_TSTRING:
      str += " -";
      str += lua_tolstring(state, -1, nullptr);
      break;
    default: // TODO: propogate error up
      lua_pushstring(state, "Incorrect type in `warnings` table");
      lua_error(state);
      return str;
    }
    --tbl_idx;
  }
  lua_pop(state, 3 + static_cast<int>(num_warnings));
  return str;
}

static auto install_exe(lua_State *state) -> int {
  fn_print();

  auto num_args = lua_gettop(state);
  if (num_args != 1) {
    lua_pushstring(state, "Too many arguments.");
    return lua_error(state);
  }

  auto main_mod = Module::make(Module::EXE, state);

  auto ec = std::error_code{};
  if (fs::create_directories(
          fs::path(std::format("{}/{}.o", main_mod.install_dir, main_mod.name)),
          ec);
      ec) {
    std::cerr << ec.message() << '\n';
    lua_pushfstring(state, "Unable to create directory");
    return lua_error(state);
  }

  // TODO: build dep tree

  // compile the objects
  auto invoked_command = std::format(
      "{} -c {} -o {}/{}.o/{}.o", main_mod.compiler, main_mod.root.c_str(),
      main_mod.install_dir, main_mod.name, main_mod.root.stem().c_str());
  std::cerr << "Invoking [" << invoked_command << "]\n";

  if (system(invoked_command.c_str()) != 0) {
    // this should be fine bc lua will intern the string(?)
    lua_pushfstring(state, "Error invoking [%s]\n", invoked_command.c_str());
    return lua_error(state);
  }

  // compile the program
  invoked_command = std::format("{} {}/{}.o/{}.o -o {}/{}", main_mod.compiler,
                                main_mod.install_dir, main_mod.name,
                                main_mod.root.stem().c_str(),
                                main_mod.install_dir, main_mod.name);
  std::cerr << "Invoking [" << invoked_command << "]\n";

  if (system(invoked_command.c_str()) != 0) {
    lua_pushfstring(state, "Error invoking [%s]\n", invoked_command.c_str());
    return lua_error(state);
  }

  exit_fn_print();
  return 0;
}

static auto install_static(lua_State *L) -> int {
  fn_print();

  auto num_args = lua_gettop(L);
  if (num_args != 1) {
    lua_pushstring(L, "Too many arguments");
    return lua_error(L);
  }

  lua_getfield(L, -1, "name");
  auto const name = string(lua_tolstring(L, -1, nullptr));

  lua_getfield(L, -2, "invoke_command");
  auto const install_command = string(lua_tolstring(L, -1, nullptr));

  expr_dbg(install_command);

  exit_fn_print();
  return 0;
}

auto make_builder_obj(lua_State *state, std::string_view const builder_obj)
    -> void {
  lua_createtable(state, 0, 2);

  lua_pushstring(state, ".");
  lua_setfield(state, -2, "install_dir");

  lua_pushcfunction(state, install_exe);
  lua_setfield(state, -2, "install_exe");

  lua_pushcfunction(state, install_static);
  lua_setfield(state, -2, "install_static");

  // TODO: add the functions install_dynamic
}
} // namespace luamake_builtins
