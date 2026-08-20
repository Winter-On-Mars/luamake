#include "luamake_builtins.hpp"

#include "common.hpp"
#include "luamake_allocator.hpp"
#include "luamake_file.hpp"
#include "luamake_pre_ir.hpp"
#include "luamake_spiral.hpp"
#include "luamake_strings.hpp"
#include "luamake_thread_pool.hpp"
#include <atomic>
#include <chrono>
#include <set>
#include <thread>

extern "C" {
#include "lua/lauxlib.h"
#include "lua/lua.h"
}

#include <array>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <endian.h>
#include <sys/wait.h>
#include <unistd.h>

#define LUA_ASSERT(L, A, B, ERROR)                                             \
  {                                                                            \
    if ((A) != (B)) {                                                          \
      lua_pushstring((L), (ERROR));                                            \
      return lua_error((L));                                                   \
    }                                                                          \
  }

#define LUA_ASSERT_FORMAT(L, name, A, B, fmt, ...)                             \
  {                                                                            \
    if (auto const name = (A); (name) != (B)) {                                \
      lua_pushfstring((L), fmt, __VA_ARGS__);                                  \
      return lua_error((L));                                                   \
    }                                                                          \
  }

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

namespace fs = std::filesystem;

// TODO: reorder things in this namespace bc things are kind of all over the
// place
// NOTE: when working with functions called directly from the lua script, we
// don't need to necessarily worry about popping from the stack to keep the
// stack clean, see the lua docs for more info
namespace luamake {
// we use the call stack as a way to keep track of this variable
// also it's not named the best(?), i can't think of a better name for it rn
// though :)
// NOTE: this variable *should* only be accessed through synchronous
// code, so no need to mutex protect it(?)
static auto previous_path = fs::path();
namespace {
// TODO: export this to the user so that they don't have to go through the
// clang/gcc functions if they don't want to
struct OptArgs final {
  std::set<std::string> args;
};

// see the lua docs about lua_type for information about how this function works
// with that
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

auto skip_ws(char const *ch) -> char const * {
  while (*ch != 0) {
    switch (*ch) {
    case ' ':
      [[fallthrough]];
    case '\t':
      [[fallthrough]];
    case '\n':
      [[fallthrough]];
    case '\r': {
      ++ch;
    } break;
    default:
      return ch;
    }
  }
  return ch;
}

// TODO: update this to sfh, check that we can
// algorithm
// https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function#FNV-1a_hash
auto constexpr fnv1a(std::span<u8 const> const bytes) noexcept -> size_t {
  auto [hash, fnv1a_prime] = []() -> std::pair<size_t, size_t> {
    if constexpr (sizeof(size_t) == 4) {
      return std::make_pair(0x01000193, 0x811c9dc5);
    } else if constexpr (sizeof(size_t) == 8) {
      return std::make_pair(0xcbf29ce484222325, 0x00000100000001b3);
    } else {
      throw;
    }
  }();

  for (size_t i{0}; i < bytes.size(); ++i) {
    hash = hash ^ static_cast<size_t>(bytes[i]);
    hash = hash * fnv1a_prime;
  }

  return hash;
}

// helper function for displaying every byte of the string_view
[[maybe_unused]]
auto display_string_view(std::string_view const str) noexcept -> void {
  for (auto i = size_t{}; i != str.length(); ++i) {
    std::cout << str[i] << '-';
  }
  std::cout << std::endl;
}

auto file_does_not_exist(fs::path &&fname, fs::path &&parent) noexcept
    -> std::runtime_error {
  if (parent == fs::current_path()) {
    return std::runtime_error(std::format(
        "Attempting to open file [{}] that does not exist.", fname.c_str()));
  } else {
    return std::runtime_error(
        std::format("Attempting to open file [{}] that does not exist, "
                    "depended on by [{}].",
                    fname.c_str(), parent.c_str()));
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

auto unexpected_char(std::string_view const ctx, char ch) noexcept
    -> std::runtime_error {
  return std::runtime_error(std::format("{} found {}", ctx, ch));
}

auto misformatted_output(std::string_view cmd) noexcept -> std::runtime_error {
  return std::runtime_error(
      std::format("When running command {}, output was not as expected", cmd));
}

auto capi(std::string &&message) noexcept -> std::runtime_error {
  return std::runtime_error(std::move(message));
}

static auto compiler_impl(lua_State *state,
                          std::string_view const compiler_name) -> int {
  // idk get better error messages
  LUA_EXPECTED_ARGUMENTS(state, 1, compiler_name);
  LUA_ASSERT(state, lua_type(state, -1), LUA_TTABLE,
             "Expected type passed to compiler function to be a table");
  auto const cc_config_idx = lua_absindex(state, -1);

  auto const path_var = std::getenv("PATH");
  if (path_var == nullptr) {
    lua_pushstring(
        state, "Envroinment variable PATH not found, you're on your own :)");
    return lua_error(state);
  }

  auto compiler_path_fut =
      std::async(std::launch::async, [path_var, compiler_name]() -> fs::path {
#if defined(_WIN32)
        auto constexpr path_sep = ';';
#else
        auto constexpr path_sep = ':';
#endif

        auto const *start = path_var;
        auto const *end = start;

        auto ec = std::error_code{};
        while (true) {
          while (*end != 0 && *end != path_sep) {
            ++end;
          }
          switch (*end) {
          case 0: {
            if (fs::exists(fs::path(start, end) / compiler_name, ec)) {
              return fs::path(start, end) / compiler_name;
            } else {
              // unable to find binary
              return fs::path();
            }
          } break;
          case path_sep: {
            if (fs::exists(fs::path(start, end) / compiler_name, ec)) {
              return fs::path(start, end) / compiler_name;
            } else {
              // binary is not is this directory
              start = end + 1; // skip the part_sep
              ++end;
            }
          } break;
          default: {
            return fs::path();
          }
          }
        }
      });

  lua_createtable(state, 0, 4);
  auto const ret_tbl_idx = lua_absindex(state, -1);

  auto *opt_args = ::new (lua_newuserdata(state, sizeof(OptArgs)))(OptArgs){};

  lua_pushnil(state);
  for (; lua_next(state, cc_config_idx) != 0;) {
    switch (lua_type(state, -2)) {
    case LUA_TNUMBER:
      LUA_ASSERT(state, lua_type(state, -1), LUA_TSTRING,
                 "Expected value type in Compiler Config to be either string "
                 "or table");
      // array values are just passed straight to the config
      opt_args->args.insert(lua_tolstring(state, -1, nullptr));
      lua_pop(state, 1);
      break;
    case LUA_TSTRING: {
      auto const field_name = lua_tolstring(state, -2, nullptr);
      switch (lua_type(state, -1)) {
      case LUA_TSTRING:
        opt_args->args.insert(std::format("-{}={}", field_name,
                                          lua_tolstring(state, -1, nullptr)));
        lua_pop(state, 1);
        break;
      case LUA_TTABLE: {
        auto const tbl_len = static_cast<lua_Integer>(lua_rawlen(state, -1));
        for (auto j = lua_Integer{1}; j <= tbl_len; ++j) {
          auto const tbl_val_t = lua_geti(state, -1, j);
          LUA_ASSERT(
              state, tbl_val_t, LUA_TSTRING,
              "Expected string in subarray passed to ha%or0\thcrah,.c&h^@cu");
          opt_args->args.insert(std::format("-{}{}", field_name,
                                            lua_tolstring(state, -1, nullptr)));
          lua_pop(state, 1);
        }
        lua_pop(state, 1);
      } break;
      default:
        lua_pushfstring(state,
                        "Unexpected value type in table to %s function. "
                        "Expected either string or table value.",
                        compiler_name.data());
        return lua_error(state);
      }
    } break;
    default:
      lua_pushstring(
          state,
          "While *yes* key's into a table can have any type, please refrain "
          "from using anything other than a number (i.e. passing an array), "
          "or "
          "a string for a key+value pair item, or a combination of the two");
      return lua_error(state);
    }
  }

  lua_setfield(state, ret_tbl_idx, "opt_args");

  auto const path_to_compiler = compiler_path_fut.get();
  if (path_to_compiler == fs::path()) {
    lua_pushfstring(state, "Unable to find %s binary", compiler_name.data());
    return lua_error(state);
  }

  lua_pushstring(state, path_to_compiler.c_str());

  lua_setfield(state, ret_tbl_idx, "compiler");

  return 1;
}

// TODO: make this throw an exception on error
// TODO: come up with a better fucking name for this
static auto default_compiler_impl(lua_State *state,
                                  std::string_view const compiler_name) -> int {
  LUA_EXPECTED_ARGUMENTS(state, 1, compiler_name);
  LUA_ASSERT_FORMAT(state, _, lua_type(state, -1), LUA_TTABLE,
                    "Expected type passed into %s function to be a table",
                    compiler_name.data());

  auto constexpr opt_level = std::string_view{"O2"};
  auto constexpr warnings = std::array<std::string_view, 3>{{
      std::string_view{"Wall"},
      std::string_view{"Wconversion"},
      std::string_view{"Wpedantic"},
  }};

  auto const arg_idx = lua_absindex(state, -1);

  auto const path_var = std::getenv("PATH");
  if (path_var == nullptr) {
    lua_pushstring(state, "Envroinment variable PATH not found, you're on your "
                          "own with this one :)");
    return lua_error(state);
  }

  // this seems slow, should benchmark it to see if it's causing the massive
  // slow down i'm noticing
  auto compiler_path_fut =
      std::async(std::launch::async, [path_var, compiler_name]() -> fs::path {
#if defined(_WIN32)
        auto constexpr path_sep = ';';
#else
        auto constexpr path_sep = ':';
#endif

        auto const *start = path_var;
        auto const *end = start;

        auto ec = std::error_code{};
        while (true) {
          while (*end != 0 && *end != path_sep) {
            ++end;
          }
          switch (*end) {
          case 0: {
            if (fs::exists(fs::path(start, end) / compiler_name, ec)) {
              return fs::path(start, end) / compiler_name;
            } else {
              // unable to find binary
              return fs::path();
            }
          } break;
          case path_sep: {
            if (fs::exists(fs::path(start, end) / compiler_name, ec)) {
              return fs::path(start, end) / compiler_name;
            } else {
              // binary is not is this directory
              start = end + 1; // skip the part_sep
              ++end;
            }
          } break;
          default: {
            return fs::path();
          }
          }
        }
      });

  lua_createtable(state, 0, 4); // tbl
  auto const ret_tbl_idx = lua_absindex(state, -1);

  lua_pushstring(state, opt_level.data());
  lua_setfield(state, ret_tbl_idx, "optimize");

  lua_createtable(state, 3, 0);
  auto constexpr table_idx = int{-2};
  for (auto idx = lua_Integer{1}; auto const warning : warnings) {
    lua_pushstring(state, warning.data());
    lua_seti(state, table_idx, idx);
    ++idx;
  }

  lua_setfield(state, ret_tbl_idx, "warnings");

  auto *opt_args = ::new (lua_newuserdata(state, sizeof(OptArgs)))(OptArgs){};

  // TODO: update this loop to be like the loop in the other compiler_impl
  // function
  lua_pushnil(state);
  for (; lua_next(state, arg_idx) != 0;) {
    switch (lua_type(state, -2)) {
    case LUA_TNUMBER: {
      LUA_ASSERT_FORMAT(
          state, _, lua_type(state, -1), LUA_TSTRING,
          "Expected value type to be a string, found something else in "
          "the %s argument",
          compiler_name.data());
      opt_args->args.insert(lua_tolstring(state, -1, nullptr));
    } break;
    case LUA_TSTRING: {
      LUA_ASSERT_FORMAT(
          state, _, lua_type(state, -1), LUA_TSTRING,
          "Expected value type to be a string, found something else in "
          "the %s argument",
          compiler_name.data());
      opt_args->args.insert(std::format("-{}={}",
                                        lua_tolstring(state, -2, nullptr),
                                        lua_tolstring(state, -1, nullptr)));
    } break;
    default:
      lua_pushstring(
          state,
          "While *yes* key's into a table can have any type, please refrain "
          "from using anything other than a number (i.e. passing an array), "
          "or "
          "a string for a key+value pair item, or a combination of the two");
      return lua_error(state);
    }
    lua_pop(state, 1);
  }

  lua_setfield(state, ret_tbl_idx, "opt_args");

  // wait until the very end to let the async function run the longest, idk if
  // this is a good thing i'm bad with async stuff
  auto const path_to_compiler = compiler_path_fut.get();
  if (path_to_compiler == fs::path()) {
    lua_pushfstring(state, "Unable to find %s binary.", compiler_name.data());
    return lua_error(state);
  }
  lua_pushstring(state, path_to_compiler.c_str());
  // lua_pushstring(state, compiler_field.data());
  lua_setfield(state, ret_tbl_idx, "compiler");

  return 1;
}

template <builtins::Module::Module_t mod_t>
auto install_impl(lua_State *state) -> int {
  if constexpr (mod_t == builtins::Module::Module_t::DYNAMIC) {
    throw std::runtime_error(
        "Currently do not support installing dynamic lib projects");
  }
  // mod_idx is at the top of the stack, and we'll just return it at the
  // end, assuming everything else has gone well
  auto const mod_idx = builtins::ModIndex(lua_tointeger(state, -1));
  if (!builtins::mods.has_module_at(mod_idx)) {
    throw std::runtime_error(
        "Attempting to install module that the system does not know "
        "about." LM_NL LM_HELP "Hint" LM_NORMAL
        ": installing a module only works "
        "when the module has been previously defined, see documentation on "
        "`new_(exe|static|dynamic)`, and/or `Git` for more information.");
  }
  auto &mod = builtins::mods.module_at(mod_idx);
  if (mod.type != mod_t) {
    throw std::runtime_error(std::format(
        "Module type is not {}, found {}",
        static_cast<std::underlying_type_t<builtins::Module::Module_t>>(mod_t),
        static_cast<std::underlying_type_t<builtins::Module::Module_t>>(
            mod.type)));
  }
  // i'm not sure if we actually need this variable now that we're using
  // everything as an absolute path but i'm not going to test that right now
  // and break everything :)
  auto const &parent_path =
      builtins::mods.get_module_path(mod_idx).parent_path();

  // NOTE: we actually have to generate the dep tree here, because we have to
  // make sure we have all of the dependency linking information at this
  // point, any sooner and we run into linking errors, everybodys favorite :)
  // TODO: try to move this into the threads structure with threads.add_task
  auto gen_dep_tree_fut = std::async(std::launch::async, [&]() -> void {
    return mod.tree.gen_dep_tree(mod, *mod.interpreter, mod_idx);
  });

  // TODO: try to move these calls to create directory to be do when the
  // initial project is set up, that way we don't have to worry about trying
  // to make them everytime which will slow things down on average
  // TODO: update these functions to throw exceptions
  // TODO: see if there is a performance increase by checking if the
  // directory is made and not making it if it is most of the time the
  // directory will be there, it's just annoying because we have to check
  // every time in case somebody changes the install_dir variable
  auto const install_dir =
      parent_path / fs::path(std::format("{}/{}.o", mod.install_dir, mod.name));
  std::cout << std::format("making directory [{}]" LM_NL, install_dir.string());
  auto ec = std::error_code{};
  if (fs::create_directories(install_dir, ec); ec) {
    // TODO: update to push the ec message
    std::cerr << ec.message() << LM_NL;
    lua_pushfstring(state, "Unable to create directory [%s]",
                    install_dir.c_str());
    return lua_error(state);
  }
  auto const cache_dir =
      fs::path(std::format("{}/__luamake_cache", mod.install_dir));
  std::cout << std::format("making directory [{}]" LM_NL, cache_dir.string());
  if (fs::create_directories(cache_dir, ec); ec) {
    std::cerr << ec.message() << LM_NL;
    lua_pushstring(state, "Unable to create directory");
    return lua_error(state);
  }

  auto const cache_path =
      fs::path(std::format("{}/{}.cache", cache_dir.string(), mod.name));
  // NOTE: we might be able to put this on a background thread, then just
  // continue on doing things, and when this is done we do the comparison,
  // but for now we'll have this be blocking :)
  auto const maybe_cached_mod = spl::deserialize(cache_path);
  auto files_to_compile = std::vector<std::string_view>();
  switch (maybe_cached_mod.index()) {
  case 0: {
    std::cout << std::format("Checking [{}] cache" LM_NL, mod.name);
    auto const &cached_mod = std::get<builtins::Module>(maybe_cached_mod);
    gen_dep_tree_fut.get();
    if (mod == cached_mod) {
      files_to_compile =
          builtins::mods.get_tree_diff(mod_idx, mod.tree, cached_mod.tree);
      if (files_to_compile.empty()) {
        std::cout << std::format("\t[{}] already built" LM_NL, mod.name);
        return 1;
      }
    } else {
      // if the mods are different, then we just have to compile them, the users
      // config changed, i.e. they specified a new macro, or a different
      // compiler, it would be nice to have additional diffing to say ok they
      // added a new macro, but only these files actually depend on said macro
      // so we only have to compile them, something to add later
      files_to_compile = mod.tree.vectorize();
    }
  } break;
  case 1: {
    // TODO: update this because it's not really an error
    auto const &error_message = std::get<std::string>(maybe_cached_mod);
    std::cerr << std::format("[{}]" LM_NL, error_message);
    gen_dep_tree_fut.get();
    // we don't have a cached tree to comp against, so we just push back
    // everything to be compiled
    files_to_compile = mod.tree.vectorize();
  } break;
  default:
    unreachable();
  }
#ifdef DEBUG_MOD
  std::cout << "[files to compile]\n\t";
  for (auto &&path : files_to_compile)
    std::cout << '[' << path << ']';
  std::cout << std::endl;
#endif // DEBUG_MOD

  threads.add_compile_tasks(files_to_compile, mod_idx);
  // TODO: have some way of keeping track of if an error occurs when
  // building a module, that way we don't try to build with extraneous
  // errors, but we still build all we can of the module for incrimental
  // builds
  threads.add_task([mod_idx, cache_path]() -> void {
    for (auto mod_state = builtins::mods.state_at(mod_idx);
         mod_state != builtins::LakeModules::ModState::ready_for_final_compile;
         mod_state = builtins::mods.state_at(mod_idx)) {
      switch (mod_state) {
      case luamake::builtins::LakeModules::ModState::error:
        // idk error, bad idea to try and compile the full module
        return;
      case luamake::builtins::LakeModules::ModState::ready_for_final_compile:
        break;
      case luamake::builtins::LakeModules::ModState::compiled:
        // idk maybe trying to compile the same module twice?
        return;
      case luamake::builtins::LakeModules::ModState::uninitialized:
        std::this_thread::sleep_for(std::chrono::nanoseconds{100});
        break;
      }
    }
    // idk maybe we could just capture parent_path(?)
    auto const &parent_path = builtins::mods.get_module_path(mod_idx);
    auto const &mod = builtins::mods.module_at(mod_idx);
    auto const compiled_files = builtins::mods.get_all_compiled_files(mod_idx);

    // only way i could think to get this to work :) *should* be optimized away
    auto const invoked_command = [&]() -> std::string {
      if constexpr (mod_t == builtins::Module::Module_t::STATIC)
        return std::format("ar crs {}/lib{}.a {}", mod.install_dir, mod.name,
                           compiled_files);
      else if (mod_t == builtins::Module::Module_t::EXE)
        return std::format("{} -o {}/{} {} {}", mod.compiler, mod.install_dir,
                           mod.name, compiled_files, mod.format_links());
    }();

    if (builtins::cl_options.verbose) {
      std::cout << std::format("[{}]" LM_NL, invoked_command);
    } else {
      if constexpr (mod_t == builtins::Module::Module_t::STATIC) {
        std::cout << std::format("Making archive for [{}]" LM_NL, mod.name);
      } else {
        std::cout << std::format("Building [{}]" LM_NL, mod.name);
      }
    }
    // NOTE: figure out how to handle errors with the lua vm, if there's
    // internal mutex's that will stop conflicting and corrupting the stack,
    // or if we have to worry about a mutex around the lua vm ourselves
    // we might have to move some of the error handling into the global
    // scope, that way we can access it across threads and communicate with
    // it through the lua vm
    // TODO: have a list of errors that we store for each module, then just
    // append to said list, at the end of compiling we can dump out a
    // summary of errors, and in verbose mode just dump out all of the
    // errors themselves, but for now just calling lua_error should be fine
    // :)
    // NOTE: the lua vm is closed after all the threads have finished, so
    // it's fine to do this, kind of, but also because this is executed
    // async, we might no longer be in the pcall function, so we really just
    // need to change how we store + handle errors :)
    if (os_call(invoked_command) != 0) {
      fprintf(stderr, "Error compiling [%s]" LM_NL, invoked_command.c_str());
      builtins::mods.set_state_at(mod_idx,
                                  builtins::LakeModules::ModState::error);
      return;
    }

    // header things that are only (currently) for static libs
    if constexpr (mod_t == builtins::Module::Module_t::STATIC) {
      fs::create_directory(
          parent_path /
          fs::path(std::format("{}/{}", mod.install_dir, mod.name)));

      auto const formatted_files = std::accumulate(
          mod.headers.begin(), mod.headers.end(), std::string(),
          [&parent_path](auto &&e, auto &&next) {
            return std::format("{} {}/{}", e,
                               parent_path.parent_path().string(),
                               next.string());
          });

      auto const copy_headers = std::format(
          "cp --target-directory={} {}",
          (parent_path / mod.install_dir / mod.name).string(), formatted_files);
      if (builtins::cl_options.verbose) {
        std::cout << std::format("[{}]" LM_NL, copy_headers);
      } else {
        std::cout << std::format("Copying [{}] headers" LM_NL, mod.name);
      }
      if (os_call(copy_headers) != 0) {
        fprintf(stderr, "Error moving headers [%s]" LM_NL,
                copy_headers.c_str());
        builtins::mods.set_state_at(mod_idx,
                                    builtins::LakeModules::ModState::error);
        return;
      }
    }
    // if everything went well then we can serialize the file
    spl::serialize(mod, cache_path);
  });
  return 1;
}

// is used for functions that don't need to actually generate the dep tree, nor
// actually call into the thread pool (i.e. cleaning up, generating the
// compile_commands.json, etc)
template <builtins::Module::Module_t mod_t>
auto install_dummy_impl(lua_State *state) -> int {
  if constexpr (mod_t == builtins::Module::Module_t::DYNAMIC) {
    throw std::runtime_error(
        "Currently do not support installing dynamic lib projects");
  }
  auto const mod_idx = builtins::ModIndex(lua_tointeger(state, -1));
  if (!builtins::mods.has_module_at(mod_idx)) {
    throw std::runtime_error(
        "Attempting to install module that the system does not know "
        "about." LM_NL LM_HELP "Hint" LM_NORMAL
        ": installing a module only works "
        "when the module has been previously defined, see documentation on "
        "`new_(exe|static|dynamic)`, and/or `Git` for more information.");
  }
  auto &mod = builtins::mods.module_at(mod_idx);
  if (mod.type != mod_t) {
    throw std::runtime_error(std::format(
        "Module type is not {}, found {}",
        static_cast<std::underlying_type_t<builtins::Module::Module_t>>(mod_t),
        static_cast<std::underlying_type_t<builtins::Module::Module_t>>(
            mod.type)));
  }
  (void)mod.tree.gen_dep_tree(mod, *mod.interpreter, mod_idx);
  return 1;
}
} // namespace

namespace builtins {
LakeModules mods = LakeModules();
CLOptions cl_options = CLOptions{};

Module::DepTree::DepTree(size_t const num_files) {
  types = std::make_unique<SourceFile_t[]>(num_files);
  files = std::make_unique<StringViews[]>(num_files);
  deps = std::make_unique<std::vector<unsigned int>[]>(num_files);
  hashes = std::make_unique<size_t[]>(num_files);

  auto const paths_size = num_files * (sizeof(char) * 15 + 1);
  auto *paths = (char *)malloc(paths_size);
  if (paths == nullptr)
    throw capi(strerror(errno));
  std::memset(paths, 0, paths_size);

  all_paths = OwnedString(paths, paths_size);

  this->num_files = 0;
  cap_files = num_files;
}

// NOTE: this function *should* only be called when we know the path has not
// been added
auto Module::DepTree::append_path(fs::path const &path) -> StringViews {
  auto const start = all_paths.size;
  all_paths.append(path.string());
  auto const end = all_paths.size;

  if (start >= std::numeric_limits<unsigned int>::max() ||
      end >= std::numeric_limits<unsigned int>::max()) {
    throw std::runtime_error(std::format(
        "Damn you have a lot of path strings, more than [{}], idk see about "
        "opening an issue to change how the indexing work, increasing the size "
        "of the StringViews class?",
        std::numeric_limits<unsigned int>::max()));
  }
#ifdef DEBUG_MOD
  std::cout << std::format(
      "File not found yet, so returning str = ({}, {})" LM_NL, start, end);
#endif // DEBUG_MOD
  return StringViews{static_cast<unsigned int>(start),
                     static_cast<unsigned int>(end)};
}

auto Module::DepTree::append_dep(Module const &mod,
                                 pp::Interpreter &interpreter,
                                 ModIndex const mod_idx,
                                 fs::path const &parent_path,
                                 fs::path const &dep, size_t const parent_idx)
    -> void {
  if (num_files == cap_files) {
    resize();
  }

  auto const possible_idx = find(dep.string());
  if (possible_idx != DepTree::NIL_IDX) {
#ifdef DEBUG_MOD
    std::cout << std::format(
        "Already found dep [{}], pushing back it's info and returning" LM_NL,
        dep.string());
#endif // DEBUG_MOD
    if (parent_idx != DepTree::NIL_IDX) {
      deps[parent_idx].push_back(static_cast<uint>(possible_idx));
    }
    return;
  }

  auto const str = append_path(dep);

  auto const this_idx = num_files;
  if (parent_idx != DepTree::NIL_IDX)
    deps[parent_idx].push_back(static_cast<unsigned int>(this_idx));
  ++num_files;

  auto file = File(parent_path / dep, File::READ);
  if (!file)
    throw file_does_not_exist(parent_path / dep, get_path(parent_idx));

  auto &&[fsize, fcontent] = file.dump_content();

  auto hash_fut = std::async(
      std::launch::async,
      [fsize](u8 const *fcontent) { return fnv1a(std::span(fcontent, fsize)); },
      fcontent.get());
  auto const ftype = DepTree::determine_file_type(dep.extension());
  if (ftype == DepTree::SourceFile_t::HEADER) {
    auto constexpr potential_extensions =
        std::array<std::string_view, 2>{{".cpp", ".c"}};
    auto const potential_impl = (dep.parent_path() / dep.stem()).string();
    for (auto const &potential_extension : potential_extensions) {
      auto const possible_path =
          fs::path(potential_impl + potential_extension.data());
      if (fs::exists(possible_path)) {
        append_dep(mod, interpreter, mod_idx, parent_path, possible_path,
                   this_idx);
      }
    }
    // HOL
  }

  types[this_idx] = ftype;
  files[this_idx] = str;

  // HACK: didn't want to rewrite all of the interpreter code to work with
  // explicitly utf8 strings
  auto const files_deps = interpreter.interpret(
      std::string_view(reinterpret_cast<char const *>(fcontent.get()), fsize),
      mods.get_allocator());

#ifdef DEBUG_MOD
  std::cout << std::format("Possible includes for {}: {{" LM_NL, dep.string());
  for (auto &&include : files_deps) {
    std::cout << std::format("\t{}" LM_NL, include.string());
  }
  std::cout << "}" LM_NL;
  std::cout.flush();

  mods.dump_paths(std::cout);
#endif // DEBUG_MOD

  auto ec = std::error_code{};
  for (auto &&file : files_deps) {
    for (auto &&include : mod.includes) {
      auto const include_rel_path =
          fs::relative(include / file, parent_path, ec);
      if (ec) {
        // idk maybe block these behind a verbose check(?)
        std::cerr << std::format("\t{}" LM_NL, ec.message());
        ec.clear();
        continue;
      }

      if (!fs::exists(include_rel_path, ec)) {
        // we should probably report an error, the issue is that we have to also
        // worry about if it's in the deps, if so then we have to worry about
        // false positives, so for now we'll just ignore things
        continue;
      }

      if (ec) {
        std::cerr << std::format("\t{}" LM_NL, ec.message());
        ec.clear();
        continue;
      }

      append_dep(mod, interpreter, mod_idx, parent_path, include_rel_path,
                 this_idx);
    }
  }

  hashes[this_idx] = hash_fut.get();
}

auto Module::DepTree::get_path(size_t const idx) const noexcept -> fs::path {
  if (idx == NIL_IDX)
    return fs::current_path();
  auto &&[start, end] = files[idx];
  // NOTE: -1 because otherwise it includes the null term, and that fucks with
  // fs::path comparing to strings and checking the extension type
  return fs::path(all_paths.buffer + start, all_paths.buffer + end - 1);
}

// TODO: update this to just use the num_files field, and the files array to
// index directly into all_paths
auto Module::DepTree::find(std::string_view const path) const noexcept
    -> size_t {
  auto const *start = all_paths.buffer;
  auto const *current = all_paths.buffer;
  auto end = size_t{};

  auto idx = size_t{};

  while (end != all_paths.size) {
    if (all_paths.buffer[end] == 0) {
      current = all_paths.buffer + end;
      auto const path_view = std::string_view{start, current};
      if (path_view.size() == path.size() &&
          *path_view.data() == *path.data() &&
          strncmp(path_view.data(), path.data(), path_view.size()) == 0) {
        return idx;
      }
      start = current + 1;
      ++idx;
    }
    ++end;
  }
  return NIL_IDX;
}

auto Module::DepTree::resize() noexcept(false) -> void {
  auto const next_cap = 3 * cap_files / 2;
  // these are basic types so we *should* just be able to memmov them
  auto n_types = std::make_unique<SourceFile_t[]>(next_cap);
  std::memmove(n_types.get(), types.get(), sizeof(SourceFile_t) * cap_files);
  auto n_files = std::make_unique<StringViews[]>(next_cap);
  std::memmove(n_files.get(), files.get(), sizeof(StringViews) * cap_files);
  auto n_hashes = std::make_unique<size_t[]>(next_cap);
  std::memmove(n_hashes.get(), hashes.get(), sizeof(size_t) * cap_files);

  auto n_deps = std::make_unique<std::vector<unsigned int>[]>(next_cap);
  for (auto i = size_t{}; i < cap_files; ++i) {
    n_deps[i] = std::move(deps[i]);
  }

  types = std::move(n_types);
  files = std::move(n_files);
  hashes = std::move(n_hashes);
  deps = std::move(n_deps);

  cap_files = next_cap;
}

auto Module::DepTree::reserve(size_t min) noexcept(false) -> void {
  if (cap_files > min) {
    return;
  }

  // these are basic types so we *should* just be able to memmov them
  auto n_types = std::make_unique<SourceFile_t[]>(min);
  auto n_files = std::make_unique<StringViews[]>(min);
  auto n_hashes = std::make_unique<size_t[]>(min);
  auto n_deps = std::make_unique<std::vector<unsigned int>[]>(min);

  std::memmove(n_types.get(), types.get(), sizeof(SourceFile_t) * cap_files);
  std::memmove(n_files.get(), files.get(), sizeof(StringViews) * cap_files);
  std::memmove(n_hashes.get(), hashes.get(), sizeof(size_t) * cap_files);
  for (auto i = size_t{}; i < cap_files; ++i) {
    n_deps[i] = std::move(deps[i]);
  }

  types = std::move(n_types);
  files = std::move(n_files);
  hashes = std::move(n_hashes);
  deps = std::move(n_deps);

  cap_files = min;
}

#ifdef DEBUG_MOD
auto Module::DepTree::display(std::ostream &out,
                              unsigned int const depth) const noexcept -> void {
  out << "All string = [" << std::string_view{all_paths.buffer, all_paths.size}
      << "]" LM_NL;
  out.flush();
  out << std::hex;
  display_impl(out, depth, 0);
  out << std::dec;
}

auto Module::DepTree::dump(std::ostream &out) const noexcept -> void {
  out << "All string = [" << std::string_view{all_paths.buffer, all_paths.size}
      << "]" LM_NL;
  out << std::format("num_files = [{}]" LM_NL, num_files);
  out << std::format("cap_files = [{}]" LM_NL, cap_files);
  for (auto i = size_t{}; i < num_files; ++i) {
    out << std::format(
        "types[{}] = [{}]" LM_NL, i,
        static_cast<std::underlying_type_t<SourceFile_t>>(types[i]));
  }
  for (auto i = size_t{}; i < num_files; ++i) {
    auto &&[start, end] = files[i];
    out << std::format("files[{}] = [{}, {}]" LM_NL, i, start, end);
  }
  for (auto i = size_t{}; i < num_files; ++i) {
    for (auto j = size_t{}; j < deps[i].size(); ++j) {
      out << std::format("deps[{}][{}] = [{}]" LM_NL, i, j, deps[i][j]);
    }
  }
  out << std::hex;
  for (auto i = size_t{}; i < num_files; ++i) {
    out << std::format("hashes[{}] = [{}]" LM_NL, i, hashes[i]);
  }
  out << std::dec;
}

auto Module::DepTree::display_impl(std::ostream &out, unsigned int const depth,
                                   unsigned int const idx) const noexcept
    -> void {
  auto const indents = std::string(depth, '\t');

  out << indents << "{" LM_NL;
  out << indents << "\"type\":\"";
  switch (types[idx]) {
  case SourceFile_t::IMPL:
    out << "IMPL";
    break;
  case SourceFile_t::HEADER:
    out << "HEADER";
    break;
  case SourceFile_t::SYSTEM:
    out << "SYSTEM";
    break;
  case SourceFile_t::MISC:
    out << "MISC";
    break;
  };
  out << "\"," LM_NL;

  out << indents << "\"path\":\""
      << std::string_view{all_paths.buffer + files[idx].start,
                          all_paths.buffer + files[idx].end}
      << "\"," LM_NL;

  out << indents << "\"hash\":" << hashes[idx] << "," LM_NL;

  out << indents << "\"deps\":[" LM_NL;

  for (auto const dep_idx : deps[idx]) {
    display_impl(out, depth + 1, dep_idx);
  }

  out << indents << "]" LM_NL;

  out << indents << "}" LM_NL;
}
#endif // DEBUG_MOD

auto Module::DepTree::vectorize() const -> std::vector<std::string_view> {
  auto res = std::vector<std::string_view>();
  res.reserve(num_files);
  for (auto i = size_t{}; i < num_files; ++i) {
    if (types[i] == Module::DepTree::SourceFile_t::IMPL) {
      res.push_back(std::string_view{all_paths.buffer + files[i].start,
                                     all_paths.buffer + files[i].end - 1});
    }
  }

  return res;
}

// TODO: optimize this, reorder equality checks, maybe in memory serialize the
// objects and just compare the bytes(?)
auto Module::operator==(Module const &that) const noexcept -> bool {
  if (type != that.type) {
    return false;
  }

  // TODO: compare interpreters, i.e. the macros

  // rest of the class
  if (compiler != that.compiler) {
    return false;
  }

  if (name != that.name) {
    return false;
  }

  if (install_dir != that.install_dir) {
    return false;
  }

  return true;
}

#ifdef DEBUG_MOD
auto Module::display(std::ostream &out) const noexcept -> void {
  auto _display = [&](auto x) { out << x << ", "; };

  out << "type = ";
  switch (type) {
  case EXE:
    out << "EXE";
    break;
  case STATIC:
    out << "STATIC";
    break;
  case DYNAMIC:
    out << "DYNAMIC";
    break;
  }

  out << LM_NL;

  out << "roots = [";
  std::for_each(roots.begin(), roots.end(), _display);
  out << "]" LM_NL;

  out << "headers = [";
  std::for_each(headers.begin(), headers.end(), _display);
  out << "]" LM_NL;

  out << "includes= [";
  std::for_each(includes.begin(), includes.end(), _display);
  out << "]" LM_NL;

  out << "dep_includes= [";
  std::for_each(dep_includes.begin(), dep_includes.end(), _display);
  out << "]" LM_NL;

  out << "sys_includes= [";
  std::for_each(sys_includes.begin(), sys_includes.end(), _display);
  out << "]" LM_NL;

  out << "linking = [";
  std::for_each(linking.begin(), linking.end(), _display);
  out << "]" LM_NL;

  out << "compiler = " << compiler << LM_NL;
  out << "name = " << name << LM_NL;
  out << "install_dir = " << install_dir << LM_NL;
  out.flush();
}
#endif // DEBUG_MOD

static auto include_path_cache =
    std::unordered_map<std::string, std::vector<fs::path>>();
// TODO: probably switch this to using popen, it seems like it will be more
// performant, plus it *should* be more portable
auto Module::append_include_paths(std::string const &compiler) -> void {
  // TODO: see if we need to cache the whole compiler string, or if it's
  // enough to just cache the path to the binary, i.e. if we can get away with
  // just caching /usr/bin/clang, then we can use the cache more, and don't
  // have to actually go into the function that much
  if (auto includes = include_path_cache.find(compiler);
      includes != include_path_cache.end()) {
    sys_includes = includes->second;
    return;
  }
  auto _pipes = std::array<int, 2>{};
  if (pipe(_pipes.data()) == -1) {
    throw capi(strerror(errno));
  }
  auto &&[read_pipe, write_pipe] = _pipes;
  auto const pid = fork();
  if (pid < 0) {
    close(read_pipe);
    close(write_pipe);
    throw capi(strerror(errno));
  }
  switch (pid) {
  case 0: { // in child proc
    // close reader
    close(read_pipe);
    auto const command_string =
        std::format("{} -v -c -xc++ /dev/null -o {}/luamake_null.o", compiler,
                    fs::temp_directory_path().c_str());

    dup2(write_pipe, STDERR_FILENO);
    if (execl("/bin/sh", "sh", "-c", command_string.c_str(), nullptr) == -1) {
      close(write_pipe); // we never actually close the write_pipe, but execl
                         // replaces the running program so idk
      throw capi(strerror(errno));
    }
  } break;
  default: { // in parent proc
    // there's probably a better way of doing this, but this is the most
    // straightforward way i can think of
    close(write_pipe);

    auto includes = std::vector<fs::path>();
    includes.reserve(10);
    auto constexpr buffer_size = sizeof(char) * size_t{2 << 8};
    auto const buffer = std::make_unique<char[]>(buffer_size + 1);
    if (buffer == nullptr)
      throw capi(strerror(errno));
    memset(buffer.get(), 0, buffer_size + 1);

    auto search_string = std::string();
    search_string.reserve(buffer_size);
    auto amount_read = read(read_pipe, buffer.get(), buffer_size);
    while (amount_read > 0) {
      search_string.append(buffer.get(), static_cast<size_t>(amount_read));
      amount_read = read(read_pipe, buffer.get(), buffer_size);
    }

    if (amount_read < 0) {
      auto const err = errno;
      // this can change errno, so to get the error we're interested in we
      // have to do this :) we might want to also check if this errors, but
      // i'll leave that for someone else to do
      close(read_pipe);
      throw capi(strerror(err));
    }
    close(read_pipe);

    // reached EOF

    auto const include_start = search_string.find("#include <");
    // TODO: update this error handling, idk probably just means we messed up
    // something with passing in the command causing the child process to error
    // out
    ASSERT_ERROR(include_start == search_string.npos);

    auto const *start_path = search_string.data() + include_start;
    while (*start_path != 0 && *start_path != '\n')
      ++start_path;

    ASSERT_ERROR(*start_path == 0);

    start_path = skip_ws(start_path);
    for (auto end_path = start_path; std::string_view{start_path, end_path} !=
                                     std::string_view{"End of search list."};
         end_path = start_path) {
      while (*end_path != 0 && *end_path != '\n') {
        ++end_path;
      }
      if (*end_path == 0)
        break;
      if (fs::exists(fs::path(std::string_view(start_path, end_path)))) {
        includes.emplace_back(fs::canonical(fs::path(start_path, end_path)));
      }
      start_path = skip_ws(end_path);
    }
    include_path_cache[compiler] = includes;
    sys_includes = includes;
    (void)waitpid(pid, nullptr, WNOHANG);
  } break;
  }
}

// TODO: add another field for function macros, so that they can be added more
// easily, and (if needed) evaluated easier
// TODO: switch this to use the pp::StringHasher, so that we can avoid some
// allocations, while also avoiding the stack use after free that comes from
// std::async call
static auto predefined_macros_cache =
    std::unordered_map<std::string, std::pair<pp::MacroMap, pp::StringSet>>();
auto Module::append_predefined_macros(std::string const &compiler)
    -> std::pair<pp::MacroMap, pp::StringSet> {
  if (auto macros = predefined_macros_cache.find(compiler);
      macros != predefined_macros_cache.end()) {
    return macros->second;
  }
  auto _pipes = std::array<int, 2>{};
  if (pipe(_pipes.data()) == -1) {
    throw capi(strerror(errno));
  }
  auto &&[read_pipe, write_pipe] = _pipes;
  auto const pid = fork();
  if (pid < 0) {
    close(read_pipe);
    close(write_pipe);
    throw capi(strerror(errno));
  }
  switch (pid) {
  case 0: { // in child proc
    // close reader
    close(read_pipe);
    auto const command_string = std::format("echo | {} -dM -E -", compiler);

    dup2(write_pipe, STDOUT_FILENO);
    if (execl("/bin/sh", "sh", "-c", command_string.c_str(), nullptr) == -1) {
      close(write_pipe); // we never actually close the write_pipe, but execl
                         // replaces the running program so idk
      throw capi(strerror(errno));
    }
  } break;
  default: { // in parent proc
    auto macros = pp::MacroMap();
    auto def_macros = pp::StringSet();
    // NOTE: based on my tests, the compiler will give about 400 macros, and <10
    // defined macros, but (at least in my experience) we end up defining a fair
    // amount of macros, basically with every file at least, hence these numbers
    // to over allocate
    macros.reserve(512);
    def_macros.reserve(32);

    close(write_pipe);
    auto *read_me =
        fdopen(dup(read_pipe), "r"); // idk saw something on stackoverflow
    auto buffer_size = sizeof(char) * size_t{2 << 8};
    auto *buffer = (char *)malloc(buffer_size + 1);
    if (buffer == nullptr)
      throw capi(strerror(errno));
    memset(buffer, 0, buffer_size + 1);

    // this seems to work, bc getline returns -1 on EOF so we can't check
    // amount_read to see if there's an error or if we just hit EOF
    errno = 0;

    auto amount_read = getline(&buffer, &buffer_size, read_me);
    for (; amount_read > 0;
         amount_read = getline(&buffer, &buffer_size, read_me)) {
      auto constexpr header = std::string_view{"#define "};
      if (strncmp(buffer, header.data(), header.size()) != 0) {
        fclose(read_me);
        free(buffer);
        close(read_pipe);
        throw misformatted_output(
            std::format("echo | {} -dM -E -",
                        std::string_view{compiler.data(), compiler.find(' ')}));
      }

      auto macro_start = header.size();
      auto macro_cur = macro_start;
      while (macro_cur < buffer_size && buffer[macro_cur] != 0) {
        if (buffer[macro_cur] == ' ' || buffer[macro_cur] == '\n') {
          break;
        }
        ++macro_cur;
      }

      // TODO: add checks to make sure we're not out of bounds, but this seems
      // to be working fine now as a hack :)
      if (buffer[macro_cur] == ' ' && buffer[macro_cur + 1] == '\n') {
        def_macros.emplace(buffer + macro_start, buffer + macro_cur - 1);
        continue;
      }

      auto const macro_name =
          std::string_view{buffer + macro_start, buffer + macro_cur};

      if (buffer[macro_cur] != ' ') {
        fclose(read_me);
        free(buffer);
        close(read_pipe);
        throw unexpected_char(
            "Defined macro ended with unexpected character, expected ' '",
            buffer[macro_cur]);
      }
      macro_start = macro_cur + 1;
      ++macro_cur;
      // TODO: find a way to actually get the macros type (int, double,
      // string_literal, etc), really seems annoying bc we could also have
      // macros defined as like functions :)
      while (macro_cur < buffer_size && buffer[macro_cur] != 0) {
        if (buffer[macro_cur] == '\n')
          break;
        ++macro_cur;
      }

      macros.emplace(macro_name, pp::Macro{std::string(buffer + macro_start,
                                                       buffer + macro_cur)});
    }

    if (errno != 0) {
      throw capi(strerror(errno));
    }

    (void)waitpid(pid, nullptr, WNOHANG);
    fclose(read_me);
    free(buffer);
    close(read_pipe);

    // reached EOF
    auto res = std::make_pair(std::move(macros), std::move(def_macros));
    predefined_macros_cache[compiler] = res;
    return res;
  } break;
  }
  unreachable();
}

// TODO: change this to just keep track of the index for the table, we don't
// need to worry about popping the values from the stack necessarily as this
// function is (mostly) called from lua itself and so we have a new stack to
// work with, see [[https://www.lua.org/manual/5.4/manual.html]] section 4 for
// more info
Module::Module(Module_t &&type, lua_State *state, fs::path const &root)
    : type(type), tree(8), roots(), headers(), includes(), sys_includes(),
      linking(), interpreter(nullptr), compiler(), name(), install_dir() {
  switch (auto const compiler_t = lua_getfield(state, -1, "compiler")) {
  case LUA_TTABLE:
    compiler = Module::parse_compiler_table(state);
    break;
  case LUA_TNIL:
    throw missing_field("compiler");
  default:
    throw unexpected_type("compiler", LUA_TTABLE, compiler_t);
  }
  lua_pop(state, 1);

  // if the compiler string doesn't contain a space then it *should* just be the
  // path to the compiler, so just use the whole length
  auto const compiler_space = compiler.find(' ') != compiler.npos
                                  ? compiler.find(' ')
                                  : compiler.length();
  auto res = std::async(std::launch::async, [this, compiler_space]() {
    auto const compiler_command = std::string(compiler.data(), compiler_space);
    this->append_include_paths(compiler_command);
  });
  auto macros_res = std::async(std::launch::async, [this, compiler_space]() {
    auto const compiler_command = std::string(compiler.data(), compiler_space);
    return this->append_predefined_macros(compiler_command);
  });

  switch (auto const name_t = lua_getfield(state, -1, "name")) {
  case LUA_TSTRING:
    name = lua_tolstring(state, -1, nullptr);
    break;
  case LUA_TNIL:
    throw missing_field("name");
  default:
    throw unexpected_type("name", LUA_TSTRING, name_t);
  }
  lua_pop(state, 1);

  switch (type) {
  case EXE:
    roots.reserve(1);
    switch (auto const root_t = lua_getfield(state, -1, "root")) {
    case LUA_TSTRING:
      roots.push_back(fs::path(lua_tolstring(state, -1, nullptr)));
      break;
    case LUA_TNIL:
      throw missing_field("root");
    default:
      throw unexpected_type("root", LUA_TSTRING, root_t);
    }
    lua_pop(state, 1);
    break;
  case STATIC: {
    // TODO: maybe we could support some kind of regex, like allowing *.c, i
    // don't like this because i think it'll lead to people including more than
    // they should, but it seems easier than writing a bunch of includes for
    // older projects that just give you everything *cough cough lua*
    switch (auto const root_t = lua_getfield(state, -1, "roots")) {
    case LUA_TTABLE: {
      auto const num_roots = lua_rawlen(state, -1);
      auto roots_tbl = -1;
      for (auto i = 1; i <= num_roots; ++i) {
        auto const value_t = lua_geti(state, roots_tbl, i);
        if (value_t != LUA_TSTRING) {
          throw unexpected_type("roots[i]", LUA_TSTRING, value_t);
        }
        roots.push_back(fs::path(lua_tolstring(state, -1, nullptr)));
        --roots_tbl;
      }
      lua_pop(state, static_cast<int>(num_roots) + 1);
    } break;
    case LUA_TNIL:
      throw missing_field("roots");
    default:
      throw unexpected_type("roots", LUA_TTABLE, root_t);
    }
  } break;
  case DYNAMIC:
    std::cerr << "Not currently implimented" LM_NL;
    std::terminate();
    break;
  }

  switch (auto const install_dir_t = lua_getfield(state, -1, "install_dir")) {
  case LUA_TSTRING:
    install_dir = (root / lua_tolstring(state, -1, nullptr)).string();
    break;
  case LUA_TNIL:
    throw missing_field("install_dir");
  default:
    throw unexpected_type("install_dir", LUA_TSTRING, install_dir_t);
  }
  lua_pop(state, 1);

  // TODO: make this a relative path
  switch (type) {
  case Module_t::EXE: {
    includes.reserve(1);
    [[likely]]
    if (roots[0].has_parent_path()) {
      includes.push_back(fs::canonical(roots[0]).parent_path());
    } else {
    }
  } break;
  case Module_t::STATIC:
    includes.reserve(roots.size());
    for (auto const &root : roots) {
      auto const parent_p = [&]() -> fs::path {
        if (root.has_parent_path()) {
          return fs::canonical(previous_path / root.parent_path());
        } else {
          return previous_path;
        }
      }();
      auto const found = std::find(includes.begin(), includes.end(), parent_p);
      if (found != includes.end()) {
        includes.push_back(parent_p);
      }
    }
  case Module_t::DYNAMIC:
    break;
  }

  switch (auto const include_t = lua_getfield(state, -1, "include")) {
  case LUA_TTABLE: {
    auto const len = lua_rawlen(state, -1);
    auto include = -1;
    for (auto i = 1; i <= len; ++i) {
      switch (auto const value_t = lua_geti(state, include, i)) {
      case LUA_TSTRING:
        includes.push_back(lua_tolstring(state, -1, nullptr));
        break;
      default:
        throw unexpected_type("include[i]", LUA_TSTRING, value_t);
      }
      --include;
    }
    lua_pop(state, static_cast<int>(len));
  } break;
  case LUA_TNIL:
    break;
  default:
    throw unexpected_type("include", LUA_TTABLE, include_t);
  }
  lua_pop(state, 1);

  // TODO: rework this, only use it for system/library includes that we
  // (luamake) doesn't control
  switch (auto const linking_t = lua_getfield(state, -1, "linking")) {
  case LUA_TTABLE: {
    auto const len = lua_rawlen(state, -1);
    linking.reserve(len);
    auto linking_idx = -1;
    for (auto i = 1; i <= len; ++i) {
      switch (auto const value_t = lua_geti(state, linking_idx, i)) {
      case LUA_TSTRING:
        linking.push_back(lua_tolstring(state, -1, nullptr));
        break;
      default:
        throw unexpected_type("linking[i]", LUA_TSTRING, value_t);
      }
      --linking_idx;
    }
    lua_pop(state, static_cast<int>(len));
  } break;
  case LUA_TNIL:
    break;
  default:
    throw unexpected_type("linking", LUA_TTABLE, linking_t);
  }
  lua_pop(state, 1);

  switch (type) {
  case Module_t::EXE:
    break;
  // TODO: make sure this isn't nil
  case Module_t::STATIC: {
    switch (auto const header_t = lua_getfield(state, -1, "headers")) {
    case LUA_TTABLE: {
      auto const len = lua_rawlen(state, -1);
      auto headers_tbl = -1;
      for (auto i = 1; i <= len; ++i) {
        switch (auto const value_t = lua_geti(state, headers_tbl, i)) {
        case LUA_TSTRING: {
          headers.push_back(fs::path(lua_tostring(state, -1)));
        } break;
        default:
          throw unexpected_type("headers[i]", LUA_TSTRING, value_t);
        }
        --headers_tbl;
      }
      lua_pop(state, static_cast<int>(len));
    } break;
    case LUA_TNIL:
      break;
    default:
      throw unexpected_type("headers", LUA_TTABLE, header_t);
    }
    lua_pop(state, 1);
  } break;
  case Module_t::DYNAMIC:
    break;
  }

  auto &&[macros, def_macros] = macros_res.get();
  switch (auto const macro_t = lua_getfield(state, -1, "macros")) {
  case LUA_TTABLE: {
    auto const len = lua_rawlen(state, -1);
    auto macros = -1;
    for (auto i = 1; i <= len; ++i) {
      auto const value_t = lua_geti(state, macros, i);
      if (value_t != LUA_TSTRING) {
        throw unexpected_type("macros[i]", LUA_TSTRING, value_t);
      }
      auto mac = std::string(lua_tolstring(state, -1, nullptr));
      if (mac.find('=') != mac.npos) {
        // TODO: parse macro being set to value
      } else {
        def_macros.insert(std::move(mac));
      }
      --macros;
    }
    lua_pop(state, static_cast<int>(len) + 1);
  } break;
  case LUA_TNIL:
    break;
  default:
    throw unexpected_type("macros", LUA_TTABLE, macro_t);
  }

  interpreter = std::make_unique<pp::Interpreter>(std::move(macros),
                                                  std::move(def_macros));
  res.get();
}

auto Module::DepTree::gen_dep_tree(Module const &mod,
                                   pp::Interpreter &interpreter,
                                   ModIndex const idx) -> void {
  auto const parent = mods.get_module_path(idx).parent_path();
  for (auto const &root : mod.roots) {
    append_dep(mod, interpreter, idx, parent, root, DepTree::NIL_IDX);
  }
}

auto Module::format_includes() const -> std::string {
  auto include_func = [](auto &&e, auto &&next) {
    return std::format("{} -I{}", e, next.string());
  };
  auto dep_func = [](auto &&e, auto &&next) {
    return std::format("{} -iquote {}", e, next.parent_path().string());
  };
  auto sys_func = [](auto &&e, auto &&next) {
    return std::format("{} -isystem {}", e, next.string());
  };
  return std::accumulate(includes.cbegin(), includes.cend(), std::string(),
                         include_func) +
         std::accumulate(dep_includes.cbegin(), dep_includes.cend(),
                         std::string(), dep_func) +
         std::accumulate(sys_includes.cbegin(), sys_includes.cend(),
                         std::string(), sys_func);
}

auto Module::format_links() const -> std::string {
  // when we add dynamic library support, we'll have to worry about the -L
  // flag and shit
  auto res = std::string();
  res.reserve(256); // idk random number can def be optimized :)
  for (auto const &path : linking) {
    res += std::format("{} ", path.string());
  }
  return res;
}

// TODO: double check that it's actually fine to throw an exception here and
// that this won't cause a memory leak, ig it's fine if it does cause a memory
// leak because this errors out to the top, but it's still a concern
auto Module::parse_compiler_table(lua_State *state) -> std::string {
  auto str = std::string();

  auto const compiler_idx = lua_absindex(state, -1);

  auto const compiler_t = lua_getfield(state, compiler_idx, "compiler");
  switch (compiler_t) {
  case LUA_TSTRING:
    str += lua_tolstring(state, -1, nullptr);
    break;
  case LUA_TNIL:
    throw missing_field("compiler.compiler");
  default:
    throw unexpected_type("compiler.compiler", LUA_TSTRING, compiler_t);
  }

  // TODO: honestly probably write a macro to make parsing optional and
  // required table entries easier
  auto const optimize_t = lua_getfield(state, compiler_idx, "optimize");
  switch (optimize_t) {
  case LUA_TSTRING:
    str += " -";
    str += lua_tolstring(state, -1, nullptr);
    break;
  case LUA_TNIL:
    break;
  default:
    throw unexpected_type("compiler.optimize", LUA_TSTRING, optimize_t);
  }

  auto const warnings_t = lua_getfield(state, compiler_idx, "warnings");
  switch (warnings_t) {
  case LUA_TTABLE: {
    auto const warnings_idx = lua_absindex(state, -1);

    lua_pushnil(state);
    while (lua_next(state, warnings_idx) != 0) {
      if (lua_type(state, -1) != LUA_TSTRING) {
        lua_pushstring(
            state,
            "Incorrect type in `warnings` table"); // TODO: update this to
                                                   // include the found type
        lua_error(state);
        return "";
      }
      str += " -";
      str += lua_tolstring(state, -1, nullptr);
      lua_pop(state, 1);
    }
  } break;
  case LUA_TNIL:
    break;
  default:
    throw unexpected_type("compiler.warnings", LUA_TTABLE, warnings_t);
  }

  auto const opt_args_t = lua_getfield(state, compiler_idx, "opt_args");
  switch (opt_args_t) {
  case LUA_TUSERDATA:
    // TODO: check metatable type
    break;
  case LUA_TNIL:
    throw missing_field("compiler.opt_args");
  default:
    throw unexpected_type("compiler.opt_args", LUA_TUSERDATA, opt_args_t);
  }
  auto *opt_args = reinterpret_cast<OptArgs *>(lua_touserdata(state, -1));
  str += std::accumulate(
      opt_args->args.begin(), opt_args->args.end(), std::string(),
      [](auto &&e, auto &&next) { return std::format("{} {}", e, next); });
  // NOTE: ok to do bc we use placement new
  opt_args->~OptArgs();

  lua_pop(state, 4);
  return str;
}

auto Builder::new_exe(lua_State *state) noexcept -> int {
  LUA_EXPECTED_ARGUMENTS(state, 2, new_exe);
  LUA_ASSERT_FORMAT(state, ret_t, lua_type(state, -1), LUA_TTABLE,
                    "Expected type of argument to `new_exe` to be of type "
                    "table, found [%s]",
                    lua_typename(ret_t));
  LUA_ASSERT_FORMAT(state, ret_t, lua_type(state, -2), LUA_TTABLE,
                    "Expected type of argument to `new_exe` to be of type "
                    "table, found [%s]",
                    lua_typename(ret_t));

  try {
    auto exe_mod =
        builtins::Module(builtins::Module::EXE, state, previous_path);
    auto const index =
        mods.append_module_with_path(previous_path, std::move(exe_mod));
    lua_pushinteger(state, static_cast<lua_Integer>(index));
    return 1;
  } catch (std::exception const &e) {
    lua_pushstring(state, e.what());
    return lua_error(state);
  } catch (...) {
    lua_pushstring(state, "Unfortunately an error occured");
    return lua_error(state);
  }
}

auto Builder::new_static(lua_State *state) noexcept -> int {
  LUA_EXPECTED_ARGUMENTS(state, 2, new_static);
  LUA_ASSERT_FORMAT(state, ret_t, lua_type(state, -1), LUA_TTABLE,
                    "Expected type of argument to `new_static` to be of type "
                    "table, found [%s]",
                    lua_typename(ret_t));
  LUA_ASSERT_FORMAT(state, ret_t, lua_type(state, -2), LUA_TTABLE,
                    "Expected type of argument to `new_static` to be of type "
                    "table, found [%s]",
                    lua_typename(ret_t));

  try {
    auto static_mod =
        builtins::Module(builtins::Module::STATIC, state, previous_path);

    auto const index =
        mods.append_module_with_path(previous_path, std::move(static_mod));
    lua_pushinteger(state, static_cast<lua_Integer>(index));
    return 1;
  } catch (std::exception const &e) {
    lua_pushstring(state, e.what());
    return lua_error(state);
  } catch (...) {
    lua_pushstring(state, "Unfortunately an error occured");
    return lua_error(state);
  }
}

auto Builder::install_exe(lua_State *state) noexcept -> int {
  LUA_EXPECTED_ARGUMENTS(state, 1, install_exe)
  LUA_ASSERT_FORMAT(state, ret_t, lua_type(state, -1), LUA_TNUMBER,
                    "Expected type of argument to `install_exe` to be of type "
                    "integer, found [%s]",
                    lua_typename(ret_t));

  try {
    return install_impl<builtins::Module::Module_t::EXE>(state);
  } catch (std::exception const &e) {
    lua_pushstring(state, e.what());
    return lua_error(state);
  } catch (...) {
    lua_pushstring(state, "Unfortunately an error occured");
    return lua_error(state);
  }
}

auto Builder::install_static(lua_State *state) noexcept -> int {
  LUA_EXPECTED_ARGUMENTS(state, 1, install_static);
  LUA_ASSERT_FORMAT(state, ret_t, lua_type(state, -1), LUA_TNUMBER,
                    "Expected type of argument to `install_static` "
                    "to be of type integer, found [%s]",
                    lua_typename(ret_t));

  try {
    return install_impl<Module::Module_t::STATIC>(state);
  } catch (std::exception const &e) {
    lua_pushstring(state, e.what());
    return lua_error(state);
  } catch (...) {
    lua_pushstring(state, "Unfortunately an error occured");
    return lua_error(state);
  }
}

auto dump_impl(lua_State *state, unsigned int const depth) noexcept -> void {
  auto const indents = [](auto depth) { return std::string(depth, '\t'); };
  switch (auto const type = lua_type(state, -1)) {
  case LUA_TNIL:
    std::cout << "nil";
    break;
  case LUA_TSTRING:
    std::cout << '"' << lua_tolstring(state, -1, nullptr) << '"';
    break;
  case LUA_TNUMBER:
    std::cout << lua_tonumber(state, -1);
    break;
  case LUA_TBOOLEAN:
    std::cout << (lua_toboolean(state, -1) ? "true" : "false");
    break;
  case LUA_TTABLE: {
    std::cout << "{" LM_NL;
    auto const tbl_idx = lua_absindex(state, -1);
    for (lua_pushnil(state); lua_next(state, tbl_idx) != 0;) {
      std::cout << indents(depth);
      switch (auto const key_t = lua_type(state, -2)) {
      case LUA_TNUMBER:
        std::cout << '[' << lua_tointeger(state, -2) << ']';
        break;
      case LUA_TSTRING:
        std::cout << '[' << lua_tolstring(state, -2, nullptr) << ']';
        break;
      default:
        std::cout << lua_typename(key_t);
        break;
      }

      std::cout << ':';
      dump_impl(state, depth + 1);

      lua_pop(state, 1);
    }
    std::cout << indents(depth - 1) << '}';
  } break;
  default:
    std::cout << lua_typename(type);
    break;
  }
  std::cout << LM_NL;
}

auto Builder::clang(lua_State *state) noexcept -> int {
  try {
    return default_compiler_impl(state, "clang");
  } catch (std::exception const &e) {
    (void)lua_pushstring(state, e.what());
    return lua_error(state);
  } catch (...) {
    (void)lua_pushstring(state, "An error occured in the clang function");
    return lua_error(state);
  }
}

// TODO: test this function
auto Builder::gcc(lua_State *state) noexcept -> int {
  try {
    return default_compiler_impl(state, "gcc");
  } catch (std::exception const &e) {
    (void)lua_pushstring(state, e.what());
    return lua_error(state);
  } catch (...) {
    (void)lua_pushstring(state, "An error occured in the gcc function");
    return lua_error(state);
  }
}

auto Builder::gcc_bare(lua_State *state) noexcept -> int {
  try {
    return compiler_impl(state, "gcc");
  } catch (std::exception const &e) {
    lua_pushstring(state, e.what());
    return lua_error(state);
  } catch (...) {
    lua_pushstring(state,
                   "An unknown exception was throw in the gcc_bare function");
    return lua_error(state);
  }
}

auto Builder::clang_bare(lua_State *state) noexcept -> int {
  try {
    return compiler_impl(state, "clang");
  } catch (std::exception const &e) {
    lua_pushstring(state, e.what());
    return lua_error(state);
  } catch (...) {
    lua_pushstring(state,
                   "An unknown exception was throw in the clang_bare function");
    return lua_error(state);
  }
}

auto Builder::require(lua_State *state) noexcept -> int {
  LUA_EXPECTED_ARGUMENTS(state, 2, requires)
  // we should probably use metatables to make sure that this table on top of
  // the stack is actually the builder table, that was something i remember
  // reading about in the best practices using the lua c api, but also that
  // would create an error later on if somebody passes in the wrong table, so
  // i don't really know that it's worth checking, like it'd just be a
  // performance hit for no reason, just assume that the user has passed in
  // the right thing, and if they haven't they'll (probably) figure it out
  // later when something breaks
  LUA_ASSERT_FORMAT(state, arg_t, lua_type(state, -2), LUA_TTABLE,
                    "Expected table to require function, found [%s]",
                    lua_typename(arg_t));
  LUA_ASSERT_FORMAT(state, arg_t, lua_type(state, -1), LUA_TSTRING,
                    "Expected string to require function, found [%s]",
                    lua_typename(arg_t));
  try {
    auto const parent_path = previous_path;
    // TODO: if the path doesn't exist, this throws, that's fine, but the error
    // message sucks, so we should check ourselves, and throw our own error if
    // that's the case
    auto const luamake_path = fs::canonical(
        parent_path /
        fs::path(std::string(lua_tolstring(state, -1, nullptr)) + ".lua"));

    auto ec = std::error_code{};
    if ((void)fs::exists(luamake_path, ec); ec) {
      // TODO: better error handling :)
      // basically just copy what we do with the main.cpp run function
      lua_pushstring(state, ec.message().c_str());
      return lua_error(state);
    }

    // for now we discard the ModIndex returned, we could instead find some
    // way to pass it along to the function, so that we don't have to
    // recompute things that we already know, idk how to do that rn though(?)
    mods.new_module(luamake_path);
    // want the builder table on top of the stack, and no longer need the path
    // name now that we have it saved as a local variable
    lua_pop(state, 1);

    if (luaL_dofile(state, luamake_path.c_str()) != LUA_OK) {
      (void)lua_pushfstring(
          state,
          "Unable to run the `luamake.lua` file required, in directory [%s]",
          luamake_path.parent_path().c_str());
      return lua_error(state);
    }

    LUA_ASSERT_FORMAT(state, ret_t, lua_type(state, -1), LUA_TTABLE,
                      "Expected table, found [%s]", lua_typename(ret_t));

    if (auto const build_func_t = lua_getfield(state, -1, "Build");
        build_func_t != LUA_TFUNCTION) {
      lua_pushfstring(state,
                      "Expected `Build` function to be a function, returned in "
                      "a table from the `luamake.lua` script at [%s]",
                      luamake_path.c_str());
      return lua_error(state);
    }

    /*
     * NOTE: currently the stack looks like this
     * [builder_obj][luamake_script_return_table][Build function]
     * so we have to rotate it to look like this
     * [idk ig the script return table][Build function][builder_obj]
     */
    lua_rotate(state, -3, -1);
    LUA_ASSERT_FORMAT(state, ret_t, lua_type(state, -1), LUA_TTABLE,
                      "Expected rotation to result in a table at the top of "
                      "the stack, found [%s]",
                      lua_typename(ret_t));
    LUA_ASSERT_FORMAT(state, ret_t, lua_type(state, -2), LUA_TFUNCTION,
                      "Expected rotation to result in a function right below "
                      "the top of the stack, found [%s]",
                      lua_typename(ret_t));

    previous_path = luamake_path.parent_path();

    // TODO: make sure that the function actually returns the integer, idk if
    // that's possible, but if so we can do that, and double check that
    // everything is actually working before commiting the changes
    if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
      return lua_error(state);
    }

    previous_path = parent_path;

    return 1;
  } catch (std::exception const &e) {
    (void)lua_pushfstring(
        state, "An exception was encountered in the requires function, [%s]",
        e.what());
    return lua_error(state);
  } catch (...) {
    (void)lua_pushstring(
        state, "An unknown exception was encountered in the requires function");
    return lua_error(state);
  }
}

auto Builder::link_lib(lua_State *state) noexcept -> int {
  LUA_EXPECTED_ARGUMENTS(state, 2, require)
  LUA_ASSERT_FORMAT(state, arg_t, lua_type(state, -2), LUA_TNUMBER,
                    "Expected integer to `link_lib` function, found [%s]",
                    lua_typename(arg_t));
  LUA_ASSERT_FORMAT(state, arg_t, lua_type(state, -1), LUA_TNUMBER,
                    "Expected integer to `link_lib` function, found [%s]",
                    lua_typename(arg_t));
  try {
    // TODO: check that the type of the modules is correct, we can link a static
    // to an exe, but we can't go the other ways, we can (when we get to dynamic
    // libs) link a static to a dynamic lib, but we have to make sure that the
    // static lib is compiled with pic (or something like that look into it),
    // etc
    auto const lib_to_be_linked = ModIndex(lua_tointeger(state, -2));
    // TODO: rename this
    auto const lib_getting_diddled = ModIndex(lua_tointeger(state, -1));

    if (!mods.has_module_at(lib_to_be_linked)) {
      throw std::runtime_error("link_lib: Attempting to link an "
                               "unknown module to another module");
    } else if (!mods.has_module_at(lib_getting_diddled)) {
      throw std::runtime_error(
          "link_lib: Attempting to link a known module to an unknown module");
    }

    auto const &mod_linked = mods.module_at(lib_to_be_linked);
    auto &mod_d = mods.module_at(lib_getting_diddled);

    // this should be correct, basically stolen from the install_static
    // function, there shouldn't be any issues, because the install_static
    // function just dumps all the headers in the same out directory
    mod_d.dep_includes.push_back(
        mods.get_module_path(lib_to_be_linked) /
        fs::path(
            std::format("{}/{}", mod_linked.install_dir, mod_linked.name)));
    mod_d.linking.push_back(fs::path(mod_linked.install_dir) /
                            ("lib" + mod_linked.name + ".a"));
    // link all the stuff that the other mod also needs
    // TODO: idk how we should check that the path is correct, because i'm
    // currently using this for system includes (-lm, -llua, -lstdc++, etc)?
    for (auto &&link : mod_linked.linking) {
      mod_d.linking.push_back(link);
    }

    return 0;
  } catch (std::exception const &e) {
    (void)lua_pushfstring(
        state, "An exception was encountered in the requires function, [%s]",
        e.what());
    return lua_error(state);
  } catch (...) {
    (void)lua_pushstring(
        state, "An unknown exception was encountered in the requires function");
    return lua_error(state);
  }
}

auto Builder::get_os(lua_State *state) noexcept -> int {
  LUA_EXPECTED_ARGUMENTS(state, 0, get_os);
  try {
    // for now we're going to be assuming that the host machine you're on is
    // the one that you're building the libraries for, i do want to add a way
    // to enable cross compilation out of the box, but i'm not sure how to do
    // that
    (void)lua_pushstring(state,
#if defined(_WIN32)
                         "windows"
#elif defined(__linux__)
                         "linux"
#elif defined(__MACH__)
                         "osx"
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) ||   \
    defined(__DragonFly__)
                         "bsd"
#endif
    );
    return 1;
  } catch (...) {
    (void)lua_pushstring(
        state, "An unknown exception was encountered in the get_os function");
    return lua_error(state);
  }
}

auto Builder::build_type(lua_State *state) noexcept -> int {
  LUA_EXPECTED_ARGUMENTS(state, 0, build_type);
  try {
    switch (cl_options.built_t) {
    case luamake::builtins::CLOptions::BuildType::dbg:
      (void)lua_pushstring(state, "debug");
      break;
    case luamake::builtins::CLOptions::BuildType::rel:
      (void)lua_pushstring(state, "release");
      break;
    case luamake::builtins::CLOptions::BuildType::dbg_w_rel:
      (void)lua_pushstring(state, "debug_and_release");
      break;
    case luamake::builtins::CLOptions::BuildType::min_rel:
      (void)lua_pushstring(state, "release_min");
      break;
    }
    return 1;
  } catch (...) {
    (void)lua_pushstring(
        state,
        "An unknown exception was encountered in the build_type function");
    return lua_error(state);
  }
}

auto Builder::install_exe_dummy(lua_State *state) noexcept -> int {
  LUA_EXPECTED_ARGUMENTS(state, 1, install_exe)
  LUA_ASSERT_FORMAT(state, ret_t, lua_type(state, -1), LUA_TNUMBER,
                    "Expected type of argument to `install_exe` to be of type "
                    "integer, found [%s]",
                    lua_typename(ret_t));
  try {
    return install_dummy_impl<builtins::Module::Module_t::EXE>(state);
  } catch (std::exception const &e) {
    lua_pushstring(state, e.what());
    return lua_error(state);
  } catch (...) {
    lua_pushstring(state, "Unfortunately an error occured");
    return lua_error(state);
  }
}

auto Builder::install_static_dummy(lua_State *state) noexcept -> int {
  LUA_EXPECTED_ARGUMENTS(state, 1, install_static)
  LUA_ASSERT_FORMAT(
      state, ret_t, lua_type(state, -1), LUA_TNUMBER,
      "Expected type of argument to `install_static` to be of type "
      "integer, found [%s]",
      lua_typename(ret_t));
  try {
    return install_dummy_impl<builtins::Module::Module_t::STATIC>(state);
  } catch (std::exception const &e) {
    lua_pushstring(state, e.what());
    return lua_error(state);
  } catch (...) {
    lua_pushstring(state, "Unfortunately an error occured");
    return lua_error(state);
  }
}

auto Runner::run(lua_State *state) noexcept -> int {
  LUA_EXPECTED_ARGUMENTS(state, 1, run);
  LUA_ASSERT_FORMAT(
      state, ret_t, lua_type(state, -1), LUA_TTABLE,
      "Expected type of argument to `run` to be of type table, found [%s]",
      lua_typename(ret_t));

  try {
    lua_getfield(state, -1, "path");
    LUA_ASSERT(state, lua_type(state, -1), LUA_TSTRING,
               "Expected type of exe.path to be string [in function Run]");
    auto exe_path = std::string(lua_tolstring(state, -1, nullptr));

    switch (auto const t = lua_getfield(state, -2, "args")) {
    case LUA_TNIL:
      // nothing to do either type is explicitly nil, or field is undefined so
      // which is fine bc it's an optional field
      break;
    case LUA_TTABLE: {
      auto const arg_idx = lua_absindex(state, -1);
      lua_pushnil(state);
      while (lua_next(state, arg_idx) != 0) {
        if (lua_type(state, -1) != LUA_TSTRING) {
          lua_pushstring(
              state, "Incorrect type in `args` table in the `run` function");
          return lua_error(state);
        }
        auto const val = lua_tolstring(state, -1, nullptr);
        exe_path += ' ';
        exe_path += val;
        lua_pop(state, 1);
      }
    } break;
    default:
      lua_pushfstring(
          state,
          "Expected type of exe.args to either be `nil` "
          "(undefined) or a table (array), found [%s] [in function Run]",
          lua_typename(t));
      return lua_error(state);
    }

    std::cout << "[" << exe_path << "]" LM_NL;
    std::cout.flush();

    os_call(exe_path);

    return 0;
  } catch (std::exception const &e) {
    lua_pushstring(state, e.what());
    return lua_error(state);
  }
}

auto dump(lua_State *state) noexcept -> int {
  auto const num_args = lua_gettop(state);
  if (num_args != 2) {
    lua_pushstring(state, std::format("Expected 2 arguments to dump function, "
                                      "[display string][value], found [{}]",
                                      num_args)
                              .c_str());
    return lua_error(state);
  }
  // don't really care about type checking the args
  switch (lua_type(state, -2)) {
  case LUA_TSTRING:
    std::cout << '[' << lua_tolstring(state, -2, nullptr) << "] = ";
    break;
  case LUA_TTABLE:
    std::cout << "[table@" << lua_topointer(state, -2) << "] = ";
    break;
  default:
    std::cout << "[Unknown type@" << lua_topointer(state, -2) << "] = ";
    break;
  }
  dump_impl(state, 1);
  return 0;
}

auto make_builder_obj(lua_State *state) noexcept -> void {
  lua_createtable(state, 0, 12);

  lua_pushcfunction(state, &Builder::clang);
  lua_setfield(state, -2, "clang");

  lua_pushcfunction(state, &Builder::gcc);
  lua_setfield(state, -2, "gcc");

  lua_pushcfunction(state, &Builder::gcc_bare);
  lua_setfield(state, -2, "gcc_bare");

  lua_pushcfunction(state, &Builder::clang_bare);
  lua_setfield(state, -2, "clang_bare");

  lua_pushcfunction(state, &Builder::new_exe);
  lua_setfield(state, -2, "new_exe");

  lua_pushcfunction(state, &Builder::new_static);
  lua_setfield(state, -2, "new_static");

  lua_pushcfunction(state, &Builder::install_exe);
  lua_setfield(state, -2, "install_exe");

  lua_pushcfunction(state, &Builder::install_static);
  lua_setfield(state, -2, "install_static");

  lua_pushcfunction(state, &Builder::require);
  lua_setfield(state, -2, "requires");

  lua_pushcfunction(state, &Builder::link_lib);
  lua_setfield(state, -2, "link_lib");

  lua_pushcfunction(state, &Builder::get_os);
  lua_setfield(state, -2, "get_os");

  lua_pushcfunction(state, &Builder::build_type);
  lua_setfield(state, -2, "build_type");

  previous_path = fs::current_path();

  // TODO: add the functions install_dynamic
}

auto make_runner_obj(lua_State *state) noexcept -> void {
  lua_createtable(state, 0, 1);

  lua_pushcfunction(state, &Runner::run);
  lua_setfield(state, -2, "run");
}

auto make_builder_dummy(lua_State *state) noexcept -> void {
  lua_createtable(state, 0, 12);

  lua_pushcfunction(state, &Builder::clang);
  lua_setfield(state, -2, "clang");

  lua_pushcfunction(state, &Builder::gcc);
  lua_setfield(state, -2, "gcc");

  lua_pushcfunction(state, &Builder::gcc_bare);
  lua_setfield(state, -2, "gcc_bare");

  lua_pushcfunction(state, &Builder::clang_bare);
  lua_setfield(state, -2, "clang_bare");

  lua_pushcfunction(state, &Builder::new_exe);
  lua_setfield(state, -2, "new_exe");

  lua_pushcfunction(state, &Builder::new_static);
  lua_setfield(state, -2, "new_static");

  lua_pushcfunction(state, &Builder::install_exe_dummy);
  lua_setfield(state, -2, "install_exe");

  lua_pushcfunction(state, &Builder::install_static_dummy);
  lua_setfield(state, -2, "install_static");

  lua_pushcfunction(state, &Builder::require);
  lua_setfield(state, -2, "requires");

  lua_pushcfunction(state, &Builder::link_lib);
  lua_setfield(state, -2, "link_lib");

  lua_pushcfunction(state, &Builder::get_os);
  lua_setfield(state, -2, "get_os");

  lua_pushcfunction(state, &Builder::build_type);
  lua_setfield(state, -2, "build_type");

  // idk, we're expecting you to be calling luamake in the same path the
  // luamake.lua file is in
  previous_path = fs::current_path();

  // TODO: add the functions install_dynamic
}

auto operator<<(std::ostream &out, ModIndex const idx) noexcept
    -> std::ostream & {
  out << idx.files << ',' << idx.mods;
  return out;
}

LakeModules::LakeModules() noexcept
    : mods_cap(0), num_mods(0), states(nullptr), compiled_files(nullptr),
      mods(nullptr), paths_cap(0), num_paths(0), luamake_paths(nullptr),
      arena(LM_EXPR_ALLOC_SIZE) {}

auto LakeModules::init(size_t const cap) -> void {
  mods_cap = static_cast<uint>(cap);
  paths_cap = static_cast<uint>(cap);
  states = std::make_unique<LakeModules::ModState[]>(cap);
  mtxs = std::make_unique<std::mutex[]>(cap);
  remaining_files = std::make_unique<std::atomic<size_t>[]>(cap);
  compiled_files = std::make_unique<std::vector<std::string>[]>(cap);
  luamake_paths = std::make_unique<fs::path[]>(cap);
  mods = std::make_unique<Module[]>(cap);

  luamake_paths[0] = fs::current_path() / "luamake.lua";
  // TODO: move these
  states[0] = ModState::uninitialized;
  remaining_files[0] = 0;
  ++num_paths;
}

auto LakeModules::deinit() -> void {
  mods_cap = paths_cap = 0;
  num_mods = num_paths = 0;
  states = nullptr;
  mtxs = nullptr;
  remaining_files = nullptr;
  compiled_files = nullptr;
  luamake_paths = nullptr;
  mods = nullptr;
}

auto LakeModules::has_module_at(ModIndex const idx) const noexcept -> bool {
  return num_mods > idx.mods && num_paths > idx.files;
}

auto LakeModules::new_module(fs::path const &path) noexcept -> void {
  if (num_paths >= paths_cap)
    resize_paths();
  luamake_paths[num_paths++] = path;
#ifdef DEBUG_MOD
  dump_paths(std::cout);
#endif // DEBUG_MOD
}

auto LakeModules::get_module_path(ModIndex const idx) const noexcept
    -> fs::path {
  return luamake_paths[idx.files];
}

auto LakeModules::emplace_at(ModIndex const idx, Module &&mod) -> void {
  mods[idx.mods] = std::move(mod);
}

auto LakeModules::module_at(ModIndex const idx) noexcept -> Module & {
  return mods[idx.mods];
}

auto LakeModules::state_at(ModIndex const idx) const noexcept -> ModState {
  auto lock = std::unique_lock(mtxs[idx.mods]);
  return states[idx.mods];
}

auto LakeModules::set_state_at(ModIndex const idx, ModState n_state) noexcept
    -> void {
  auto lock = std::unique_lock(mtxs[idx.mods]);
  states[idx.mods] = n_state;
}

auto LakeModules::add_compiled_file(ModIndex const idx,
                                    std::string &&str) noexcept -> void {
  auto lock = std::unique_lock(mtxs[idx.mods]);
  auto &lof = compiled_files[idx.mods];
  auto const &mod = mods[idx.mods];
#ifdef DEBUG_MOD
  std::cout << LM_HELP "[pushing back]" LM_NORMAL
            << std::format("[{}/{}.o/{}.o]", mod.install_dir, mod.name, str)
            << " to " << idx << std::endl;
#endif // DEBUG_MOD
  lof.emplace_back(
      std::format("{}/{}.o/{}.o", mod.install_dir, mod.name, std::move(str)));
  remaining_files[idx.mods]--;
  // this should work(?), and should mean that this is the last file that was
  // needed to be compiled(?)
  // if this doesn't end up working, then we will need to probably have two
  // numbers, one that keeps track of the number of files compiled, and the
  // other that says how many files total we need to compile, then compare
  // those two number(?)
  if (remaining_files[idx.mods] == 0) {
    states[idx.mods] = ModState::ready_for_final_compile;
  }
}

auto LakeModules::get_tree_diff(ModIndex const mod_idx,
                                Module::DepTree const &tree,
                                Module::DepTree const &cache_tree)
    -> std::vector<std::string_view> {
  auto files_to_compile = std::vector<std::string_view>();
  files_to_compile.reserve(tree.num_files);
  auto already_compiled = std::vector<std::string_view>();
  already_compiled.reserve(tree.num_files);
  for (auto i = size_t{}; i < tree.num_files; ++i) {
    if (tree.types[i] != Module::DepTree::SourceFile_t::IMPL) {
      continue;
    }
    // -1 bc otherwise it includes the null terminator
    auto const cur_file =
        std::string_view{tree.all_paths.buffer + tree.files[i].start,
                         tree.all_paths.buffer + tree.files[i].end - 1};
    auto const that_cur_file = cache_tree.find(cur_file);
    if (that_cur_file == Module::DepTree::NIL_IDX) {
#ifdef DEBUG_MOD
      std::cout << "Could not find `" << cur_file << "` pushing back\n";
#endif // DEBUG_MOD
      files_to_compile.push_back(cur_file);
    } else if (cache_tree.hashes[that_cur_file] != tree.hashes[i]) {
#ifdef DEBUG_MOD
      std::cout << "File `" << cur_file << "` has a different hash\n";
#endif // DEBUG_MOD
      files_to_compile.push_back(cur_file);
    } else {
      already_compiled.push_back(cur_file);
    }
  }
#ifdef DEBUG_MOD
  std::cout << "[already_compiled]\n\t";
  for (auto &&f : already_compiled) {
    std::cout << '[' << f << ']';
  }
  std::cout << std::endl;
#endif // DEBUG_MOD
  // vectorized unsafe add already compiled files
  auto lock = std::unique_lock(mtxs[mod_idx.mods]);
  auto &lof = compiled_files[mod_idx.mods];
  auto const &mod = mods[mod_idx.mods];
  // NOTE: file *should* be relative to the luamake.lua file, but we just want
  // the file name, this *should* work but is a bit of a HACK
  for (auto &&file : already_compiled) {
    auto const fname = [&file]() -> std::string_view {
      auto const pos = file.rfind('/');
      if (pos == file.npos) {
        return file;
      } else {
        // + 1 bc file[pos] == '/'
        return std::string_view(file.cbegin() + pos + 1, file.cend());
      }
    }();
#ifdef DEBUG_MOD
    std::cout << LM_HELP "[pushing back]" LM_NORMAL
              << std::format("[{}/{}.o/{}.o]", mod.install_dir, mod.name, fname)
              << " to " << mod_idx << std::endl;
#endif // DEBUG_MOD
    lof.emplace_back(
        std::format("{}/{}.o/{}.o", mod.install_dir, mod.name, fname));
  }
  return files_to_compile;
}

auto LakeModules::init_allocator() noexcept -> void { arena.init(); }

auto LakeModules::get_allocator() noexcept -> allocator::Page & {
  return arena;
}

auto LakeModules::get_all_compiled_files(ModIndex const idx) noexcept
    -> std::string {
  auto lock = std::unique_lock(mtxs[idx.mods]);
  auto const &vec = compiled_files[idx.mods];
  return std::accumulate(
      vec.begin(), vec.end(), std::string(),
      [](auto &&a, auto &&next) { return std::format("{} {}", a, next); });
}

auto LakeModules::append_module_with_path(fs::path const &path, Module &&mod)
    -> ModIndex {
  if (num_mods >= mods_cap) {
    resize_mods();
  }
  mods[num_mods] = std::move(mod);
  auto const mods = num_mods;
  ++num_mods;
  auto files = ModIndex::not_found;
  for (auto i = uint{}; i < num_paths; ++i) {
    if (luamake_paths[i] == path / "luamake.lua") {
      files = i;
    }
  }
  if (files == ModIndex::not_found) {
#ifdef DEBUG_MOD
    dump_paths(std::cout);
#endif // DEBUG_MOD
    throw std::runtime_error(
        std::format("Unable to associate module with path [{}], path in the "
                    "known luamake paths",
                    path.string()));
  }
  return ModIndex(files, mods);
}

auto LakeModules::resize_mods() -> void {
  for (auto i = size_t{}; i < num_mods; ++i)
    mtxs[i].lock();
  auto const n_cap = 3 * mods_cap / 2;

  auto n_states = std::make_unique<LakeModules::ModState[]>(n_cap);
  auto n_mtxs = std::make_unique<std::mutex[]>(n_cap);
  auto n_remaining_files = std::make_unique<std::atomic<size_t>[]>(n_cap);
  auto n_compiled_files = std::make_unique<std::vector<std::string>[]>(n_cap);
  auto n_mods = std::make_unique<Module[]>(n_cap);

  std::memcpy(n_states.get(), states.get(), sizeof(bool) * mods_cap);

  for (auto i = size_t{}; i < mods_cap; ++i)
    n_remaining_files[i] = std::move(static_cast<size_t>(remaining_files[i]));
  for (auto i = size_t{}; i < mods_cap; ++i)
    n_compiled_files[i] = std::move(compiled_files[i]);
  for (auto i = size_t{}; i < mods_cap; ++i)
    n_mods[i] = std::move(mods[i]);

  states = std::move(n_states);
  remaining_files = std::move(n_remaining_files);
  compiled_files = std::move(n_compiled_files);
  mods = std::move(n_mods);
  // idk if we should have a mutex for this array of mutices (?), would that
  // defeat the point of having multiple mutexs to allow the parallel code to
  // not interfear with one another(?)
  for (auto i = size_t{}; i < num_mods; ++i)
    mtxs[i].unlock();
  // idk if mutexs actually contain any reference data that we should be
  // worried about, i think if we just make a new array with the new size it
  // should be fine
  mtxs = std::move(n_mtxs);
}

auto LakeModules::resize_paths() -> void {
  // idk what happens if an exception is thrown here, that seems like one of
  // those unrecoverable exceptions (i.e. out of memory) so we just give up
  // anyways
  for (auto i = size_t{}; i < num_mods; ++i) {
    mtxs[i].lock();
  }
  auto const n_cap = 3 * paths_cap / 2;
  auto n_luamake_paths = std::make_unique<fs::path[]>(n_cap);

  for (auto i = size_t{}; i < paths_cap; ++i)
    n_luamake_paths[i] = std::move(luamake_paths[i]);

  luamake_paths = std::move(n_luamake_paths);
  for (auto i = size_t{}; i < num_mods; ++i) {
    mtxs[i].unlock();
  }
}

#ifdef DEBUG_MOD
auto LakeModules::dump_paths(std::ostream &out) const noexcept -> void {
  out << "modules.paths = {" LM_NL;
  for (auto i = uint{0}; i < num_paths; ++i) {
    out << "\t[" << i << "][" << luamake_paths[i].string() << "]" LM_NL;
  }
  out << "}" LM_NL;
  out.flush();
}

auto LakeModules::dump_modules(std::ostream &out) const noexcept -> void {
  out << "modules.mods = {" LM_NL;
  for (auto i = uint{}; i < num_mods; ++i) {
    out << '[' << i << "] {";
    mods[i].display(out);
    out << '}';
  }
  out << "}" LM_NL;
  out.flush();
}
#endif // DEBUG_MOD
} // namespace builtins
} // namespace luamake
#undef LUA_ASSERT
#undef LUA_ASSERT_FORMAT
#undef LUA_EXPECTED_ARGUMENTS
