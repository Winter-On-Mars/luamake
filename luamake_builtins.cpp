#include "luamake_builtins.hpp"

auto dump_impl(lua_State *state, int const idx) noexcept -> std::string {
  switch (lua_type(state, idx)) {
  case LUA_TNONE:
    lua_error(state);
    return std::string(); // (?)
  case LUA_TNIL:
    return std::string("nil");
  case LUA_TBOOLEAN:
    return lua_toboolean(state, idx) == 1 ? std::string("true")
                                          : std::string("false");
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
