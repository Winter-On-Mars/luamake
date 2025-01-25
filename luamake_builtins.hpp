#ifndef __LUAMAKE_BUILTINS_HPP
#define __LUAMAKE_BUILTINS_HPP

#include <array>
#include <string_view>

#include <lua.hpp>

#include "common.hpp"

namespace luamake_builtins {
static auto clang(lua_State *state) -> int {
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

  return 1;
}

auto dump_impl(lua_State *state, int const idx) noexcept -> std::string {
  switch (lua_type(state, idx)) {
  case LUA_TNONE:
    lua_error(state);
    return std::string(); // (?)
  case LUA_TNIL:
    return std::string("nil");
  case LUA_TBOOLEAN:
    return lua_toboolean(state, idx) == 1 ? std::string("true") : std::string("false");
  case LUA_TLIGHTUSERDATA: {
    auto res = std::string(luaL_tolstring(state, idx, nullptr));
    lua_pop(state, 1);
    return res;
  } break;
  case LUA_TNUMBER:
    return std::to_string(lua_tonumber(state, idx));
  case LUA_TSTRING:
    return std::string(lua_tostring(state, idx));
  case LUA_TTABLE: {
    auto res = std::string("Work in progress on dumping tables values\n");
    res += '{';
    res += '\n';
    res += '\t';

    lua_pushnil(state);
    while (lua_next(state, -1) != 0) {
      res += dump_impl(state, -2);
      res += dump_impl(state, -1);
      lua_pop(state, 1); // would be better to have a variable in the while loop
      // so that we don't have a bunch of this stack manip going on
    }
    lua_pop(state, 1);

    res += '}';
    res += '\n';
  } break;
  case LUA_TFUNCTION: {
    auto res = std::string("<lua: fn>");
    res += ' ';
    res += luaL_tolstring(state, idx, nullptr);
    lua_pop(state, 1);
    return res;
  } break;
  case LUA_TUSERDATA: {
    auto res = std::string(luaL_tolstring(state, idx, nullptr));
    lua_pop(state, 1);
    return res;
  } break;
  case LUA_TTHREAD: {
    auto res = std::string("<lua: thread>");
    res += std::string(luaL_tolstring(state, idx, nullptr));
    lua_pop(state, 1);
    return res;
  } break;
  }
  unreachable();
}

static auto dump(lua_State *state) -> int {
  fn_print();

  auto const num_args = lua_gettop(state);
  if (num_args != 1) { // idk if this is actually how to do error handling (?)
    lua_pushnil(state);
    return 1;
  }

  auto res = std::string();

  switch (lua_type(state, -1)) {
  case LUA_TNONE:
    lua_error(state);
    return 1; // (?)
    break;
  case LUA_TNIL:
    res = std::string("nil");
    break;
  case LUA_TBOOLEAN:
    res = lua_toboolean(state, -1) == 1 ? std::string("true") : std::string("false");
    break;
  case LUA_TLIGHTUSERDATA:
    luaL_tolstring(state, -1, nullptr);
    return 1;
    break;
  case LUA_TNUMBER:
    res = std::to_string(lua_tonumber(state, -1));
    break;
  case LUA_TSTRING:
    res = std::string(lua_tostring(state, -1));
    break;
  case LUA_TTABLE:
    res += std::string("Work in progress on dumping tables values\n");
    res += '{';
    res += '\n';
    res += '\t';

    lua_pushnil(state);
    while (lua_next(state, -1) != 0) {
      res += dump_impl(state, -2);
      res += dump_impl(state, -1);
      lua_pop(state, 1); // would be better to have a variable in the while loop
      // so that we don't have a bunch of this stack manip going on
    }
    lua_pop(state, 1);

    res += '}';
    res += '\n';
    break;
  case LUA_TFUNCTION:
    res = std::string("<lua: fn>");
    break;
  case LUA_TUSERDATA:
    luaL_tolstring(state, -1, nullptr);
    return 1;
    break;
  case LUA_TTHREAD:
    res = std::string("<lua: thread>");
    res += std::string(luaL_tolstring(state, -1, nullptr));
    lua_pop(state, 1);
    break;
  }

  lua_pushstring(state, res.c_str());
  return 1;
}

constexpr luaL_Reg const funcs[] = {
    {"clang", clang}, {"dump", dump}, {nullptr, nullptr}};
} // namespace luamake_builtins

#endif
