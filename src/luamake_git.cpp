#include "luamake_git.hpp"
#include "common.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <type_traits>

extern "C" {
#include "lua/lauxlib.h"
#include "lua/lua.h"
}

namespace luamake::builtins {
namespace {
namespace fs = std::filesystem;

// TODO: maybe extract these into their own file because they're used both by
// luamake_git and luamake_builtins

#define LUA_EXPECTED_ARGUMENTS(L, expected_args, fn_name)                      \
  {                                                                            \
    auto const num_args = lua_gettop(L);                                       \
    if (num_args != expected_args) {                                           \
      lua_pushfstring(                                                         \
          L, "Expected " #expected_args " arguments to " #fn_name ", got %d.", \
          num_args);                                                           \
      return lua_error(L);                                                     \
    }                                                                          \
  }

#define LUA_ASSERTF(L, name, A, B, fmt, ...)                                   \
  {                                                                            \
    if (auto const name = (A); (name) != (B)) {                                \
      lua_pushfstring((L), fmt, __VA_ARGS__);                                  \
      return lua_error((L));                                                   \
    }                                                                          \
  }

auto constexpr lua_typename(int const type) noexcept -> char const * {
  switch (type) {
  case LUA_TNIL:
    return "LUA_TNIL";
  case LUA_TBOOLEAN:
    return "LUA_TBOOLEAN";
  case LUA_TLIGHTUSERDATA:
    return "LUA_TLIGHTUSERDATA";
  case LUA_TNUMBER:
    return "LUA_TNUMBER";
  case LUA_TSTRING:
    return "LUA_TSTRING";
  case LUA_TTABLE:
    return "LUA_TTABLE";
  case LUA_TFUNCTION:
    return "LUA_TFUNCTION";
  case LUA_TUSERDATA:
    return "LUA_TUSERDATA";
  case LUA_TTHREAD:
    return "LUA_TTHREAD";
  default:
    return "LUA_NONE";
  }
}

auto missing_field(std::string_view const field_name) noexcept
    -> std::runtime_error {
  return std::runtime_error(std::format(
      "Required field [{}] could not be found when constructing a module.",
      field_name));
}

auto unexpected_type(std::string_view const field_name, int expected_type,
                     int found_type) noexcept -> std::runtime_error {
  return std::runtime_error(std::format(
      "Required field [{}] found, but was of type {}, "
      "expected type {}, when constructing a module.",
      field_name, lua_typename(found_type), lua_typename(expected_type)));
}

enum class InstallLevel {
  project,
  user,
};

// NOTE: name needs to be null terminated
template <class T>
[[nodiscard]]
auto constexpr get_field(lua_State *state, int idx, std::string_view const name)
    -> T {
  using act_t = std::remove_cvref_t<T>;
  auto const t_t = lua_getfield(state, idx, name.data());
  if (t_t == LUA_TNIL)
    throw missing_field(name);
  if constexpr (std::is_same_v<act_t, std::string>) {
    if (t_t == LUA_TSTRING) {
      auto const ret_t = std::string(lua_tolstring(state, -1, nullptr));
      lua_pop(state, 1);
      if (ret_t.empty()) {
        throw std::runtime_error(
            std::format("Attempting to get field {}, found blank", name));
      }
      return ret_t;
    } else {
      throw unexpected_type(name, LUA_TSTRING, t_t);
    }
  } else if constexpr (std::is_same_v<act_t, InstallLevel>) {
    if (t_t == LUA_TSTRING) {
      auto const level = std::string_view{lua_tolstring(state, -1, nullptr)};
      lua_pop(state, 1);
      if (level == std::string_view{"project"}) {
        return InstallLevel::project;
      } else if (level == std::string_view{"user"}) {
        return InstallLevel::user;
      } else {
        throw std::runtime_error(std::format(
            "Attempting to git.clone at an unknown install level {}", level));
      }
    } else {
      throw unexpected_type(name, LUA_TSTRING, t_t);
    }
  } else {
    throw std::runtime_error("Not impl");
  }
  unreachable();
}

template <class T>
[[nodiscard]]
auto constexpr get_opt_field(lua_State *state, int idx,
                             std::string_view const name)
    -> std::optional<std::remove_cvref_t<T>> {
  using act_t = std::remove_cvref_t<T>;
  auto const t_t = lua_getfield(state, idx, name.data());
  if (t_t == LUA_TNIL) {
    lua_pop(state, 1);
    return std::optional<T>();
  }
  if constexpr (std::is_same_v<act_t, std::string>) {
    if (t_t == LUA_TSTRING) {
      auto const ret_t = std::string(lua_tolstring(state, -1, nullptr));
      // pop field from stack so other calls can just use -1 as well
      lua_pop(state, 1);
      return std::optional<std::string>(ret_t);
    } else {
      throw unexpected_type(name, LUA_TSTRING, t_t);
    }
  } else if constexpr (std::is_same_v<act_t, bool>) {
    if (t_t == LUA_TBOOLEAN) {
      auto const res = lua_toboolean(state, -1);
      lua_pop(state, 1);
      return std::optional<bool>(res);
    } else {
      throw unexpected_type(name, LUA_TBOOLEAN, t_t);
    }
  } else {
    throw std::runtime_error("Not impl");
  }
  unreachable();
}

auto install_project_locally(std::string_view const name,
                             std::string_view const url,
                             std::string_view const branch, bool shallow)
    -> void {
  // TODO: either have a global value defined in some shared header file between
  // this and luamake_builtins, or have a value in the lua_State* to do the same
  // thing, because we also need this to be relative to the current luamake
  // file, not cwd necessarily
  if (fs::exists(fs::current_path() / name)) {
    std::cout << std::format("local package {0:} already installed, continuing "
                             "(if there is an error, remove the directory {0:} "
                             "and the git clone command will run)\n",
                             name);
    return;
  }
  // layout based on the man page for git-clone
  // need to figure out a better way to handle the jobs (used when cloning
  // submodules)
  auto const branch_fmt = branch != std::string_view{""}
                              ? std::format("-b {}", branch)
                              : std::string("");
  auto const shallow_fmt = shallow ? "--depth=1" : "";
  auto const cmd_str =
      std::format("git clone {} {} --shallow-submodules -j2 -- {} {}",
                  branch_fmt, shallow_fmt, url, name);
  expr_dbg(cmd_str);
  auto *clone = popen(cmd_str.c_str(), "r");
  if (!clone)
    throw std::runtime_error(
        std::format("Unable to clone project `{}` on branch `{}` from `{}`",
                    name, branch, url));
  pclose(clone);
}

auto clone(lua_State *state) noexcept -> int {
  LUA_EXPECTED_ARGUMENTS(state, 1, git.clone);
  LUA_ASSERTF(
      state, type_t, lua_type(state, -1), LUA_TTABLE,
      "Expected type of argument to `git.clone` to be of type table, found %s",
      lua_typename(state, type_t));
  try {
    auto const name = get_field<std::string>(state, -1, "name");
    auto const url = get_field<std::string>(state, -1, "url");
    // there could be an argument made for having this be optional, but i think
    // doing so could lead to unexpected issues, plus we have to worry about
    // what to do if you want to lock a project, that's something we'll have to
    // get into at some point :(
    auto const branch = get_field<std::string>(state, -1, "branch");
    auto const shallow =
        get_opt_field<bool>(state, -1, "shallow").value_or(true);
    auto const project_level =
        get_field<InstallLevel>(state, -1, "install_level");
    switch (project_level) {
    case InstallLevel::project:
      install_project_locally(name, url, branch, shallow);
      break;
    case InstallLevel::user:
      throw std::runtime_error("Installing packages on a user level is "
                               "currently unsupported, will be added soon");
    }

    // TODO: see note in install_project_locally about this
    auto const path_to_proj = fs::current_path() / name;
    lua_pushstring(state, path_to_proj.c_str());
    return 1;
  } catch (std::exception const &err) {
    lua_pushstring(state, err.what());
    return lua_error(state);
  } catch (...) {
    lua_pushstring(state, "Unfortunately, an unknown error has occured");
    return lua_error(state);
  }
}
#undef LUA_EXPECTED_ARGUMENTS
#undef LUA_ASSERTF

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
