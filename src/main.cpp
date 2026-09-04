#include "common.hpp"
#include "luamake_allocator.hpp"
#include "luamake_builtins.hpp"
#include "luamake_file.hpp"
#include "luamake_git.hpp"
#include "luamake_thread_pool.hpp"

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <numeric>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <thread>
#include <type_traits>

#ifdef __unix__
#include <fcntl.h>
#include <unistd.h>
#endif

extern "C" {
#include "lua/lauxlib.h"
#include "lua/lua.h"
#include "lua/lualib.h"
}

namespace fs = std::filesystem;

#ifndef LM_LUA_ALLOC_SIZE
#define LM_LUA_ALLOC_SIZE 2 << 12
#endif

namespace {
enum class Value_t { NUMBER, STRING, BOOL_TRUE, BOOL_FALSE, NIL };
auto constexpr determine_type(std::string_view const str) noexcept -> Value_t {
  if (str.length() == 0 || str == std::string_view{"nil"}) {
    return Value_t::NIL; // i don't know if this is actually what we want to do?
  } else if (str == std::string_view{"true"}) {
    return Value_t::BOOL_TRUE;
  } else if (str == std::string_view{"false"}) {
    return Value_t::BOOL_FALSE;
  } else if (isalpha(str[0])) {
    return Value_t::STRING;
  } else if (isdigit(str[0])) {
    return Value_t::NUMBER;
  }
  return Value_t::NIL;
}

// returns -1 if '=' char is not found
auto constexpr find_eq(char const *str) -> ssize_t {
  for (auto i = size_t{}; str[i] != '\0'; ++i) {
    if (str[i] == '=')
      return static_cast<ssize_t>(i);
  }
  return -1;
}

auto constexpr cstrlen(char const *str) noexcept -> size_t {
  auto pos = size_t{};
  for (;;) {
    if (str[pos] == '\0')
      break;
    else
      ++pos;
  }
  return pos;
}

auto constexpr matches(char const *a, std::string_view const b) noexcept
    -> bool {
  return (cstrlen(a) == b.length()) && strncmp(a, b.data(), b.length()) == 0;
}

auto constexpr partial_matches(char const *a, std::string_view const b) noexcept
    -> bool {
  return (cstrlen(a) > b.length()) && strncmp(a, b.data(), b.length()) == 0;
}

auto get_cl_args(lua_State *state, int argc, char **argv) noexcept -> void {
  auto start_lua_args = 0;
  for (; start_lua_args < argc; ++start_lua_args) {
    if (matches(argv[start_lua_args], std::string_view{"-v"}) ||
        matches(argv[start_lua_args], std::string_view{"--verbose"})) {
      luamake::builtins::cl_options.verbose = true;
    } else if (partial_matches(argv[start_lua_args],
                               std::string_view{"-nthreads="})) {
      // NOTE: (Winter-On-Mars) sizeof on the string literal includes the null
      // terminator, so we have to subtract that off
      auto const *num_start = argv[start_lua_args] + sizeof("-nthreads=") - 1;
      auto n_threads = size_t{};
      auto const n_match = sscanf(num_start, "%zu", &n_threads);
      if (n_match != 1) {
        fwarning_message("Unable to read number of threads specified using all "
                         "allowed minus 1." LM_NL LM_HELP "Hint" LM_NORMAL
                         ": expected format string `-nthreads=%%zu`, got `%s`",
                         argv[start_lua_args]);
        luamake::builtins::cl_options.num_threads =
            std::thread::hardware_concurrency() - 1;
      } else {
        luamake::builtins::cl_options.num_threads =
            n_threads > std::thread::hardware_concurrency()
                ? std::thread::hardware_concurrency()
                : n_threads;
      }
    } else if (matches(argv[start_lua_args],
                       std::string_view{"--everything"})) {
      luamake::builtins::cl_options.clean_everything = true;
    } else if (matches(argv[start_lua_args],
                       std::string_view{"--executable"})) {
      luamake::builtins::cl_options.proj_t =
          luamake::builtins::CLOptions::ProjectType::executable;
    } else if (matches(argv[start_lua_args], std::string_view{"--static"})) {
      luamake::builtins::cl_options.proj_t =
          luamake::builtins::CLOptions::ProjectType::static_;
    } else if (matches(argv[start_lua_args], std::string_view{"--dynamic"})) {
      luamake::builtins::cl_options.proj_t =
          luamake::builtins::CLOptions::ProjectType::dynamic;
    } else if (matches(argv[start_lua_args], std::string_view{"--"})) {
      ++start_lua_args;
      break;
    }
  }

  lua_createtable(state, 0, argc - start_lua_args);
  for (; start_lua_args < argc; ++start_lua_args) {
    auto arg = argv[start_lua_args];
    auto const eq_pos = find_eq(argv[start_lua_args]);
    if (eq_pos == ssize_t{-1}) {
      fwarning_message("Arguments passed to the args table should be of the "
                       "form <arg_name>=<arg_value>, here arg_value is a lua "
                       "literal value (no spaces between the '=')." LM_NL
                       "\tIgnoring arg_name = %s",
                       arg);
      continue;
    }
    // NOTE: (Winter-On-Mars) we have to do this bc lua_setfield internally
    // calls strlen, looking for a '\0'
    arg[eq_pos] = '\0';
    auto const arg_name = arg;
    auto const value_str = arg + eq_pos + 1;
    switch (determine_type(value_str)) {
    case Value_t::NUMBER: {
      // the function handles both integers and floats depending on lua lex
      // rules
      auto val = lua_stringtonumber(state, value_str);
      if (val == 0) {
        // idk report an error
        lua_pushnil(state);
      }
    } break;
    case Value_t::STRING:
      lua_pushstring(state, value_str);
      break;
    case Value_t::BOOL_TRUE:
      lua_pushboolean(state, true);
      break;
    case Value_t::BOOL_FALSE:
      lua_pushboolean(state, false);
      break;
    case Value_t::NIL:
      lua_pushnil(state);
      break;
    }
    lua_setfield(state, -2, arg_name);
  }
  lua_setglobal(state, "args");
}

// <lua vm stuff>
struct LuaError final {
  std::string message;
};

// TODO: update this to throw, we then need to update our api functions to
// accept thrown objects
[[maybe_unused]] [[noreturn]]
auto throw_panic(lua_State *state) -> int {
  const char *msg = (lua_type(state, -1) == LUA_TSTRING)
                        ? lua_tostring(state, -1)
                        : "error object is not a string";
  throw LuaError{
      std::format("PANIC: unprotected error in call to Lua API ({})\n", msg)};
  // return 0; /* return to Lua to abort */
}

// from lua/lauxlib.c 1041
auto std_panic(lua_State *L) -> int {
  const char *msg = (lua_type(L, -1) == LUA_TSTRING)
                        ? lua_tostring(L, -1)
                        : "error object is not a string";
  lua_writestringerror("PANIC: unprotected error in call to Lua API (%s)\n",
                       msg);
  return 0; /* return to Lua to abort */
}

// TODO: (Winter-On-Mars) SECURITY concerns, review each module and see if there
// are any that we **need** to get rid of, and if so, if there are some features
// that can be useful that we should provide though our own mock std lib
auto constexpr supported_libs = std::array<luaL_Reg, 9>{
    luaL_Reg{LUA_GNAME, luaopen_base},
    luaL_Reg{LUA_LOADLIBNAME, luaopen_package},
    luaL_Reg{LUA_COLIBNAME,
             luaopen_coroutine}, // this might cause some issues, because we
                                 // haven't really thought about what will
                                 // happen when coroutines are running, but for
                                 // now i'll leave it in
    luaL_Reg{LUA_TABLIBNAME, luaopen_table},
    luaL_Reg{LUA_IOLIBNAME, luaopen_io}, // should probably remove(?)
    // big security issue, though we should probably expose some of these, like
    // os.clock
    // {LUA_OSLIBNAME, luaopen_os},
    luaL_Reg{LUA_STRLIBNAME, luaopen_string},
    luaL_Reg{LUA_MATHLIBNAME, luaopen_math},
    luaL_Reg{LUA_UTF8LIBNAME, luaopen_utf8},
    //  {LUA_DBLIBNAME, luaopen_debug},
    luaL_Reg{"git", &luamake::builtins::luaopen_git},
    // luaL_Reg{NULL, NULL}
};
// see linit.c 57
auto open_libs(lua_State *state) -> void {
  for (auto &&[name, func] : supported_libs) {
    luaL_requiref(state, name, func, 1);
    lua_pop(state, 1); // remove lib
  }
}
// </lua vm stuff>

enum class exit_t : unsigned char {
  ok,
  internal_error,
  lua_vm_error,
  config_error,
  useage_error,
};

static auto new_proj(std::string_view const,
                     luamake::builtins::CLOptions::ProjectType const) noexcept
    -> exit_t;
static auto lua_ls(std::filesystem::path &&) noexcept -> exit_t;
static auto help() noexcept -> exit_t;

static auto build(lua_State *const) noexcept -> exit_t;
static auto clean(lua_State *const, bool const) noexcept -> exit_t;
static auto compile_commands_json(lua_State *const) noexcept -> exit_t;
static auto run(lua_State *const) noexcept -> exit_t;
static auto test(lua_State *const) noexcept -> exit_t;

enum class Command : int {
  UNKNOWN_ARG,
  BUILD,
  NEW,
  CLEAN,
  TEST,
  RUN,
  CC_JSON,
  LUA_LS,
  HELP,
};

auto determine_command(int argc, char **argv) noexcept -> Command {
  if (argc == 1) {
    return Command::RUN;
  }

  if (matches(argv[1], "b") || matches(argv[1], "build")) {
    return Command::BUILD;
  } else if (matches(argv[1], "n") || matches(argv[1], "new")) {
    if (argc < 3) {
      error_message("Expected string for the name of the "
                    "project, found nothing" LM_NL
                    "\tDisplaying help message for more information" LM_NL);
      return Command::HELP;
    }
    return Command::NEW;
  } else if (matches(argv[1], "c") || matches(argv[1], "clean")) {
    return Command::CLEAN;
  } else if (matches(argv[1], "t") || matches(argv[1], "test")) {
    return Command::TEST;
  } else if (matches(argv[1], "r") || matches(argv[1], "run")) {
    return Command::RUN;
  } else if (matches(argv[1], "-h") || matches(argv[1], "--help")) {
    return Command::HELP;
  } else if (matches(argv[1], "cc") || matches(argv[1], "compile_commands")) {
    return Command::CC_JSON;
  } else if (matches(argv[1], "luals")) {
    if (argc < 3) {
      error_message("Expected string for the dir to write the luals files, "
                    "found nothing" LM_NL
                    "\tDisplaying help message for more information" LM_NL);
      return Command::HELP;
    }
    return Command::LUA_LS;
  } else {
    fwarning_message("Unknown argument [%s]" LM_NL
                     "\tDisplaying help for list of accepted arguments",
                     argv[1]);
    return Command::UNKNOWN_ARG;
  }
}

auto run_command(Command const command, int argc, char **argv) noexcept
    -> exit_t {
  switch (command) {
  case Command::UNKNOWN_ARG:
    return help();
  case Command::NEW: {
    return new_proj(std::string_view{argv[2]},
                    luamake::builtins::cl_options.proj_t);
  }
  case Command::LUA_LS:
    return lua_ls(argv[2]);
  case Command::HELP:
    return help();
  case Command::CLEAN:
    [[fallthrough]];
  case Command::CC_JSON:
    [[fallthrough]];
  case Command::BUILD:
    [[fallthrough]];
  case Command::TEST:
    [[fallthrough]];
  case Command::RUN:
    break;
  }

  auto page_allocator = luamake::allocator::Page(LM_LUA_ALLOC_SIZE);
  auto *state = lua_newstate(page_allocator.to_lua_alloc(), &page_allocator);
  if (state == nullptr) {
    error_message(
        "Unable to init luavm." LM_NL
        "\tThere may be some issue with your lua lib, if "
        "not feel free to message me on discord/ open an issue on the "
        "gh");
    return exit_t::lua_vm_error;
  }
  // can probably remove this after we get things working
  lua_atpanic(state, &std_panic);
  // NOTE: there's some bug with -fsanitize=memory and fs::current_path, known
  // bug, says it's fixed in clang21(?) but that's what i'm testing on so idk
  auto lake =
      luamake::File(fs::current_path() / "luamake.lua", luamake::File::READ);
  if (!lake) {
    ferror_message(
        "Unable to discover `luamake.lua` in current dir at [%s]" LM_NL "\tRun "
        "init <proj-name> to create a initialize a new project, "
        "or new <proj-name> to create a new subproject.",
        fs::current_path().c_str());
    return exit_t::config_error;
  }
  (void)lua_gc(state, LUA_GCSTOP);

  open_libs(state);
  lua_register(state, "Dump", luamake::builtins::dump);

  get_cl_args(state, argc, argv);

  auto &&[len, str] = lake.dump_content();
  // basically the same thing as the luaL_dostring macro, but we just have the
  // buffer already
  if ((luaL_loadbufferx(state, reinterpret_cast<char const *>(str.get()), len,
                        "luamake.lua", nullptr) ||
       lua_pcall(state, 0, 0, 0)) != LUA_OK) {
    ferror_message("unable to run the discovered `luamake.lua` file at "
                   "[%s]" LM_NL "\tLua error message [%s]",
                   fs::current_path().c_str(), lua_tostring(state, -1));
    return exit_t::config_error;
  }

  luamake::builtins::mods.init();
  luamake::threads.init(luamake::builtins::cl_options.num_threads);

  auto res = exit_t::ok;
  switch (command) {
  case Command::BUILD:
    res = build(state);
    break;
  case Command::TEST:
    res = test(state);
    break;
  case Command::RUN:
    res = run(state);
    break;
  case Command::CLEAN: {
    res = clean(state, luamake::builtins::cl_options.clean_everything);
  } break;
  case Command::CC_JSON:
    res = compile_commands_json(state);
    break;
  case Command::LUA_LS:
    [[fallthrough]];
  case Command::UNKNOWN_ARG:
    [[fallthrough]];
  case Command::NEW:
    [[fallthrough]];
  case Command::HELP:
    unreachable();
  }
  luamake::threads.deinit();
  luamake::builtins::mods.deinit();
  lua_close(state);
  return res;
}

static auto
new_proj(std::string_view const project_name,
         luamake::builtins::CLOptions::ProjectType const type) noexcept
    -> exit_t {
  auto const project_root = fs::current_path() / project_name;

  if (fs::exists(project_root)) {
    ferror_message("Project [%s] already exists at [%s]" LM_NL "\tExiting",
                   project_name.data(), project_root.c_str());
    return exit_t::useage_error;
  }

  // create the dir
  fs::create_directory(project_root);

  // create build + src dirs
  fs::create_directory(project_root / "build");
  fs::create_directory(project_root / "src");

  // creating default `luamake.lua`
  auto luamake_lua =
      luamake::File(project_root / "luamake.lua",
                    luamake::File::WRITE | luamake::File::CREATE);
  if (!luamake_lua) {
    ferror_message("Unable to open file at [%s]." LM_NL
                   "\tThis could be an issue "
                   "with permissions, or out of space.",
                   (fs::current_path() / "luamake.lua").c_str());
    return exit_t::internal_error;
  }

  // NOTE: these are all format strings, so they need to be passed to
  // std::format
  auto constexpr lua_f_content = std::array<std::string_view, 3>{
      // clang-format off
    std::string_view{"function Build(b)" LM_NL
                     "    local exe = b:new_exe({{" LM_NL
                     "        name = \"{0}\"," LM_NL
                     "        root = \"src/main.cpp\"," LM_NL
                     "        compiler = b.clang({{}})," LM_NL
                     "        version = \"0.0.1\"," LM_NL
                     "        install_dir = \"build\"," LM_NL
                     "        linking = {{ \"stdc++\" }}," LM_NL
                     "    }})" LM_NL
                     LM_NL
                     "    return b.install_exe(exe)" LM_NL
                     "end" LM_NL
                     LM_NL
                     "function Run(r)" LM_NL
                     "    local exe = {{" LM_NL
                     "        name = \"{0}\"," LM_NL
                     "        path = \"build/{0}\"," LM_NL
                     "        args = {{}}," LM_NL
                     "    }}" LM_NL
                     LM_NL
                     "    r.run(exe)" LM_NL
                     "end" LM_NL
                     LM_NL
                     "Tests = {{" LM_NL
                     "    {{" LM_NL
                     "        fun = function(t)" LM_NL
                     "            t.exe = \"build/{0}\"" LM_NL
                     "            t.args = {{\"This does nothing\"}}" LM_NL
                     "        end," LM_NL
                     "        output = {{" LM_NL
                     "            expected = \"Hello World!\\n\"," LM_NL
                     "            from = \"stdout\"," LM_NL
                     "        }}," LM_NL
                     "    }}" LM_NL
                     "}}" LM_NL},
      std::string_view{"local function Build(b)" LM_NL
                  "    local dlib = b:new_dynamic({{" LM_NL
                  "        roots = {{ \"src/dyn.cpp\" }}," LM_NL
                  "        headers = {{ \"src/dyn.hpp\" }}," LM_NL
                  "        compiler = b.clang({{}})," LM_NL
                  "        name = \"{0}\"," LM_NL
                  "        version = \"0.0.1\"," LM_NL
                  "        install_dir = \"build\"," LM_NL
                  "    }})" LM_NL
                  "    return b.install_dynamic(dlib)" LM_NL
                  "end" LM_NL
                  LM_NL
                  "return {{" LM_NL
                  "    Build = Build" LM_NL
                  "}}"
                  },
      std::string_view{"local function Build(b)" LM_NL
                  "    local slib = b:new_static({{" LM_NL
                  "        roots = {{ \"src/static.cpp\" }}," LM_NL
                  "        headers = {{ \"src/static.hpp\" }}," LM_NL
                  "        compiler = b.clang({{}})," LM_NL
                  "        name = \"{0}\"," LM_NL
                  "        version = \"0.0.1\"," LM_NL
                  "        install_dir = \"build\"," LM_NL
                  "    }})" LM_NL
                  "    return b.install_static(slib)" LM_NL
                  "end" LM_NL
                  LM_NL
                  "return {{" LM_NL
                  "    Build = Build" LM_NL
                  "}}"
                  },
      // clang-format on
  };

  auto const actual_string =
      std::vformat(lua_f_content[static_cast<std::underlying_type_t<
                       luamake::builtins::CLOptions::ProjectType>>(type)],
                   std::make_format_args(project_name));

  if (luamake_lua.write(actual_string.c_str(), actual_string.size(), 1) !=
      actual_string.size()) {
    ferror_message("Unable to write full luamake template string into the lua "
                   "file at [%s]",
                   (project_root / "luamake.lua").c_str());
    return exit_t::internal_error;
  }

  luamake_lua.flush();

  auto constexpr file_paths =
      std::array<std::pair<std::string_view, std::string_view>, 3>{
          std::pair("", "src/main.cpp"),
          std::pair("src/dyn.hpp", "src/dyn.cpp"),
          std::pair("src/static.hpp", "src/static.cpp"),
      };

  auto constexpr hpp_cpp_f_content =
      std::array<std::pair<std::string_view, std::string_view>, 3>{
          std::pair(std::string_view{""},
                    std::string_view{
                        ""
                        // clang-format off
               "#include <iostream>" LM_NL
               LM_NL
               "auto main() -> int {" LM_NL
               "    using std::cout;" LM_NL
               "    cout << \"Hello World!\" << std::endl;" LM_NL
               "}" LM_NL
                        // clang-format on
                    }),
          std::pair(
              std::string_view{
                  ""
                  // clang-format off
              "#pragma once" LM_NL
              LM_NL
              "namespace dlib {" LM_NL
              "[[nodiscard]]" LM_NL
              "auto call_me(int) noexcept -> int;" LM_NL
              "}" LM_NL
                  // clang-format on
              },
              std::string_view{
                  ""
                  // clang-format off
              "#include \"dyn.hpp\"" LM_NL
              LM_NL
              "namespace dlib {" LM_NL
              "[[nodiscard]]" LM_NL
              "auto call_me(int i) noexcept -> int {" LM_NL
              "    return i + 1;" LM_NL
              "}" LM_NL
              "}" LM_NL
                  // clang-format on
              }),
          std::pair(
              std::string_view{
                  ""
                  // clang-format off
              "#pragma once" LM_NL
              LM_NL
              "namespace slib {" LM_NL
              "[[nodiscard]]" LM_NL
              "auto call_me(int) noexcept -> int;" LM_NL
              "}" LM_NL
                  // clang-format on
              },
              std::string_view{
                  ""
                  // clang-format off
              "#include \"static.hpp\"" LM_NL
              LM_NL
              "namespace slib {" LM_NL
              "[[nodiscard]]" LM_NL
              "auto call_me(int i) noexcept -> int {" LM_NL
              "    return i + 1;" LM_NL
              "}" LM_NL
              "}" LM_NL
                  // clang-format on
              }),
      };

  auto &&[header_f_name, impl_f_name] = file_paths[static_cast<
      std::underlying_type_t<luamake::builtins::CLOptions::ProjectType>>(type)];

  auto &&[header_string, impl_string] = hpp_cpp_f_content[static_cast<
      std::underlying_type_t<luamake::builtins::CLOptions::ProjectType>>(type)];

  auto *header = (!header_f_name.empty())
                     ? fopen((project_root / header_f_name).c_str(), "w")
                     : nullptr;
  if (type != luamake::builtins::CLOptions::ProjectType::executable &&
      header == nullptr) {
    ferror_message("Unable to open file at [%s]." LM_NL
                   "\tThis could be an issue "
                   "with permissions, or out of space.",
                   (project_root / header_f_name).c_str());
    return exit_t::internal_error;
  }

  auto impl = luamake::File(project_root / impl_f_name,
                            luamake::File::WRITE | luamake::File::CREATE);
  if (!impl) {
    ferror_message("Unable to open file at [%s]." LM_NL
                   "\tThis could be an issue "
                   "with permissions, or out of space.",
                   (project_root / impl_f_name).c_str());
    return exit_t::internal_error;
  }

  if (type != luamake::builtins::CLOptions::ProjectType::executable &&
      fprintf(header, "%s", header_string.data()) != header_string.length()) {
    ferror_message("Unable to write full hpp file template string at [%s]",
                   (project_root / header_f_name).c_str());
    fclose(header);
    return exit_t::internal_error;
  }

  if (impl.write(impl_string.data(), impl_string.size(), 1) !=
      impl_string.size()) {
    ferror_message("Unable to write full cpp file template string at [%s]",
                   (project_root / impl_f_name).c_str());
    fclose(header);
    return exit_t::internal_error;
  }

  if (header != nullptr)
    fclose(header);

  return exit_t::ok;
}

static auto lua_ls(fs::path &&dir) noexcept -> exit_t {
  if (!fs::exists(dir))
    std::filesystem::create_directories(dir);
  fs::create_directory(dir / "library");

  // creating default `luamake.lua`
  auto config_json = luamake::File(
      dir / "config.json", luamake::File::WRITE | luamake::File::CREATE);
  if (!config_json) {
    ferror_message("Unable to open file at [%s]." LM_NL
                   "\tThis could be an issue "
                   "with permissions, or out of space.",
                   (fs::current_path() / "config.json").c_str());
    return exit_t::internal_error;
  }

  auto constexpr config_json_content = std::string_view{
      // clang-format off
  "{" LM_NL
    "\"$schema\": \"https://raw.githubusercontent.com/LuaLS/LLS-Addons/main/schemas/addon_config.schema.json\"," LM_NL
    "\"words\": [" LM_NL
      "\"function Build%(%s%)\"" LM_NL
    "]," LM_NL
    "\"files\": [" LM_NL
      "\"luamake.lua\"" LM_NL
    "]," LM_NL
    "\"settings\": {" LM_NL
      "\"Lua.workspace.library\": [" LM_NL
        "\"${3rd}/luamake/library\"" LM_NL
      "]" LM_NL
    "}" LM_NL
  "}"
      // clang-format on
  };
  if (config_json.write(config_json_content.data(),
                        config_json_content.length(),
                        1) != config_json_content.length()) {
    ferror_message("Unable to write full config.json at [%s]", dir.c_str());
    return exit_t::internal_error;
  }
  config_json.flush();

  auto constexpr luamake_lua_content = std::string_view{
      R"0(---@meta luamake

---@class ExeConfig
---@field name string
---@field root string
---@field compiler CompilerConfig
---@field install_dir string
---@field version string?
---@field include string[]?
---@field linking string[]?
---@field macros string[]?

---@class LibConfig
---@field name string
---@field roots string[]
---@field headers string[]
---@field compiler CompilerConfig
---@field install_dir string
---@field version string?
---@field include string[]?
---@field linking string[]?
---@field macros string[]?

---@alias CCOptions table<string, string|table<string,string>>

---@class CompilerConfig
---@field compiler string
---@field opt_args table<any, any>
---@field optimize string?
---@field warnings string[]?

---@class LibType

---@class DepConfig
---@field where string
---@field threads integer
---@field commands string[]
---@field expecting LibType

---@alias ModuleIndex integer

---@alias OsType 'windows'|'linux'|'osx'|'bsd'|nil

---@alias BuildType 'release'|'debug'|'debug_and_release'|'release_min'

---@class BuildCtx
---@field requires fun(self: BuildCtx, path: string): ModuleIndex
---@field new_exe fun(self: BuildCtx, config: ExeConfig): ModuleIndex
---@field new_static fun(self: BuildCtx, config: LibConfig): ModuleIndex
---@field new_dynamic fun(self: BuildCtx, config: LibConfig): ModuleIndex
---@field link_lib fun(library: ModuleIndex, link_to: ModuleIndex): nil
---@field install_exe fun(mod: ModuleIndex): ModuleIndex
---@field install_static fun(mod: ModuleIndex): ModuleIndex
---@field install_dynamic fun(mod: ModuleIndex): ModuleIndex
---@field install_dep fun(self: BuildCtx, config: DepConfig): ModuleIndex
---@field clang fun(cc_options: CCOptions): CompilerConfig
---@field gcc fun(cc_options: CCOptions): CompilerConfig
---@field gcc_bare fun(cc_options: CCOptions): CompilerConfig
---@field clang_bare fun(cc_options: CCOptions): CompilerConfig
---@field cmake fun(commands: string[]): string[]
---@field get_os fun(): OsType
---@field build_type fun(): BuildType

---@class RunConfig
---@field name string
---@field path string
---@field args string[]?

---@class RunCtx
---@field run fun(runable_config: RunConfig): integer

---@class TestCtx
---@field set_exe fun(self: TestCtx, command: string)
---@field add_arg fun(self: TestCtx, arg: string)
---@field add_args fun(self: TestCtx, args: string[])
---@field expect_success fun(self: TestCtx)
---@field expect_failure fun(self: TestCtx)
---@field expect_output fun(self: TestCtx, output: string, from_fd?: 'stdout' | 'stderr')

---@param name string name of value
---@param arg any value will be recursively displayed to stdout
function Dump(name, arg) end

---@type table<string, string>
args = {}

---@class CloneConfig
---@field name string
---@field url string
---@field branch string
---@field shallow ?boolean Default=true
---@field install_level 'project'

git = {
	---@param config CloneConfig
	---@return string
	clone = function(config) end,
})0"};

  auto luamake_lua =
      luamake::File(dir / "library/luamake.lua",
                    luamake::File::WRITE | luamake::File::CREATE);
  if (!luamake_lua) {
    ferror_message("Unable to open file at [%s]." LM_NL "\t" LM_HELP
                   "HINT" LM_NORMAL ": This could be an issue "
                   "with permissions, or out of space.",
                   (fs::current_path() / "library/luamake.lua").c_str());
    return exit_t::internal_error;
  }
  if (luamake_lua.write(luamake_lua_content.data(),
                        luamake_lua_content.length(),
                        1) != luamake_lua_content.length()) {
    ferror_message(
        "Unable to write full luamake.lua file for luals support at [%s]",
        (dir / "library").c_str());
    return exit_t::internal_error;
  }

  luamake_lua.flush();

  return exit_t::ok;
}

static auto help() noexcept -> exit_t {
  // clang-format off
  printf(
      "Usage: luamake [options]?" LM_NL
      "options:" LM_NL
      "\t-h, --help                          : Displays this help message." LM_NL
      "\tc, clean                            : Cleans the cache dir and removes the output." LM_NL
      "\tcc, compile_commands                : Generates `compile_commands.json` file in `install_dir`, defined in the respective `luamake.lua` file." LM_NL
      "\tluals <dir>                         : Generates LuaLS project files in <dir>." LM_NL
      "\tn, new <project-name> [project-args]: Creates a new subdir with name <project-name>, "
      "creating a default luamake build script." LM_NL
      "\ti, init <project-name> [init-args]  :" LM_NL
      "\tb, build                            : Builds the project based on the `Build` function "
      "defined in the `luamake.lua` file in the current dir." LM_NL
      "\tt, test                             : Builds the project based on the `Build` function "
      "in the `luamake.lua` file in the current dir, with the additional macro "
      "`LUAMAKE_TESTS` defined. Then runs the tests defined in the `Test` "
      "function "
      "defined in the `luamake.lua` file in the current dir, displaying the "
      "number of tests that succeeded." LM_NL
      "\tr, run                              : Builds the project based on the `Build` function "
      "defined in the `luamake.lua` file in the current dir. Then runs the "
      "program, based on the `Run` function defined in the current dirs "
      "`luamake.lua` file." LM_NL
      "If no options are passed in, it is the same as calling `luamake -r`" LM_NL
      "For more information see the `README.md` at "
      "[[https://github.com/Winter-On-Mars/luamake]]" LM_NL);
  // clang-format on
  fflush(stdout);
  return exit_t::ok;
}

static auto build(lua_State *const state) noexcept -> exit_t {
  auto const build_lua_fn = lua_getglobal(state, "Build");
  // function undefined in `luamake.lua`
  if (build_lua_fn == LUA_TNIL) {
    error_message(
        "Unable to find function `Build` in discovered `luamake.lua`." LM_NL
        "\tSee README/wiki for more info");
    return exit_t::config_error;
  }
  // value Build is defined as a global, but isn't a function
  if (build_lua_fn != LUA_TFUNCTION) {
    error_message("`Build` value found in `luamake.lua`, but is not a "
                  "function (might be callable [why would you do that?])." LM_NL
                  "\tSee README/wiki for more info, and if is a callable, feel "
                  "free to open a gh issue to fix this problem (and maybe "
                  "explain why the code's formatted this way lol)");
    return exit_t::config_error;
  }

  auto const builder = lua_getglobal(state, luamake::BUILDER_OBJ);
  switch (builder) {
  case LUA_TNIL:
    lua_pop(state, 1);
    luamake::builtins::make_builder_obj(state);
    break;
  case LUA_TTABLE:
    break;
  default:
    ferror_message("`builder` object was defined, but it's type was expected "
                   "to be table, got [%s]",
                   lua_typename(state, lua_type(state, -1)));
    return exit_t::config_error;
  }

  if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
    auto const err_message = lua_tolstring(state, -1, nullptr);
    ferror_message("While in the lua vm, Build function" LM_NL "\t%s",
                   err_message);
    return exit_t::lua_vm_error; // ?
  }

  return exit_t::ok;
}

static auto clean(lua_State *const state, bool const rm_everything) noexcept
    -> exit_t {
  auto const build_fn_t = lua_getglobal(state, "Build");
  switch (build_fn_t) {
  case LUA_TFUNCTION:
    break;
  case LUA_TNIL:
    error_message(
        "Unable to find function `Build` in discovered `luamake.lua`." LM_NL
        "\tSee README/wiki for more info");
    return exit_t::config_error;
  default:
    error_message("`Build` value found in `luamake.lua`, but is not a "
                  "function (might be callable [why would you do that?])." LM_NL
                  "\tSee README/wiki for more info, and if is a callable, feel "
                  "free to open a gh issue to fix this problem (and maybe "
                  "explain why the code's formatted this way lol)");
    return exit_t::config_error;
  }

  // normally we need to get the builder object from the global, but in this
  // case there's no other point that can call this function, so we just need to
  // make a builder object
  luamake::builtins::make_builder_dummy(state);
  if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
    auto const err_message = lua_tolstring(state, -1, nullptr);
    ferror_message("While in the lua vm, Build function" LM_NL "\t[%s]",
                   err_message);
    return exit_t::lua_vm_error; // ?
  }

  for (auto &&mod : luamake::builtins::mods) {
    if (mod.tree.is_empty()) {
      continue;
    }
    auto const cache_path = fs::path(
        std::format("{}/__luamake_cache/{}.cache", mod.install_dir, mod.name));
    (void)fs::remove(cache_path);
    if (rm_everything) {
      auto const object_path =
          fs::path(std::format("{}/{}.o", mod.install_dir, mod.name));
      for (auto &&obj : fs::directory_iterator(object_path)) {
        (void)fs::remove(obj);
      }
    }
  }
  return exit_t::ok;
}

static auto compile_commands_json(lua_State *const state) noexcept -> exit_t {
  auto const build_fn_t = lua_getglobal(state, "Build");
  switch (build_fn_t) {
  case LUA_TFUNCTION:
    break;
  case LUA_TNIL:
    error_message(
        "Unable to find function `Build` in discovered `luamake.lua`." LM_NL
        "\tSee README/wiki for more info");
    return exit_t::config_error;
  default:
    error_message("`Build` value found in `luamake.lua`, but is not a "
                  "function (might be callable [why would you do that?])." LM_NL
                  "\tSee README/wiki for more info, and if is a callable, feel "
                  "free to open a gh issue to fix this problem (and maybe "
                  "explain why the code's formatted this way lol)");
    return exit_t::config_error;
  }

  // normally we need to get the builder object from the global, but in this
  // case there's no other point that can call this function, so we just need to
  // make a builder object
  luamake::builtins::make_builder_dummy(state);
  if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
    auto const err_message = lua_tolstring(state, -1, nullptr);
    ferror_message("While in the lua vm, Build function" LM_NL "\t[%s]",
                   err_message);
    return exit_t::lua_vm_error; // ?
  }

  for (auto &&mod : luamake::builtins::mods) {
    // HACK: when dealing with external modules, modules that are built using
    // another build system, they don't have a dep tree, thus it will be empty
    if (mod.tree.is_empty()) {
      continue;
    }
    auto const &directory = mod.install_dir;
    auto const arguments = [&]() -> std::string {
      auto res = std::string();
      res.reserve(1024);
      auto prev = size_t{};
      auto i = size_t{};
      for (; i < mod.compiler.size(); ++i) {
        if (mod.compiler[i] == ' ') {
          res.append(1, '"');
          res.append(mod.compiler.substr(prev, i - prev));
          res.append(1, '"');
          res.append(1, ',');
          prev = i + 1;
        }
      }
      res.append(1, '"');
      res.append(mod.compiler.substr(prev, i - prev));
      res.append(1, '"');
      res.append(1, ',');
      return res;
    }();

    auto const includes =
        std::accumulate(mod.includes.cbegin(), mod.includes.cend(),
                        std::string(), [](auto &&a, auto &&next) {
                          return std::format("{}\"-I{}\",", a, next.string());
                        });
    auto const sys_includes = std::accumulate(
        mod.sys_includes.cbegin(), mod.sys_includes.cend(), std::string(),
        [](auto &&a, auto &&next) {
          return std::format("{}\"-isystem\",\"{}\",", a, next.string());
        });

    auto cc_json_string = std::string(1, '[');
    for (auto i = size_t{}; i < mod.tree.size() - 1; ++i) {
      cc_json_string.append("{");
      cc_json_string.append(std::format("\"directory\":\"{}\",", directory));

      cc_json_string.append("\"arguments\":[");
      cc_json_string.append(arguments);

      cc_json_string.append(includes);
      cc_json_string.append(sys_includes);

      cc_json_string.append("\"-c\",\"-o\",");
      auto const fname = mod.tree.get_path(i).stem().string();
      auto const obj_path =
          std::format("{}/{}.o/{}.o", mod.install_dir, mod.name, fname);
      auto const fpath = mod.tree.get_path(i);
      cc_json_string.append(
          std::format("\"{}\",\"{}\"", obj_path.c_str(), fpath.c_str()));
      cc_json_string.append("],");

      // for some reason we can't use the .string method on the file path,
      // because it includes the null terminator
      cc_json_string.append(
          std::format("\"file\":\"{}\"", mod.tree.get_path(i).c_str()));
      cc_json_string.append("},");
    }
    // generate the last module
    cc_json_string.append("{");
    cc_json_string.append(std::format("\"directory\":\"{}\",", directory));

    cc_json_string.append("\"arguments\":[");
    cc_json_string.append(arguments);
    cc_json_string.append(includes);
    cc_json_string.append(sys_includes);

    cc_json_string.append("\"-c\",\"-o\",");
    auto const fname = mod.tree.get_path(mod.tree.size() - 1).stem().string();
    auto const obj_path =
        std::format("{}/{}.o/{}.o", mod.install_dir, mod.name, fname);
    auto const fpath = mod.tree.get_path(mod.tree.size() - 1);
    cc_json_string.append(
        std::format("\"{}\",\"{}\"", obj_path.c_str(), fpath.c_str()));
    cc_json_string.append("],");

    cc_json_string.append(std::format(
        "\"file\":\"{}\"", mod.tree.get_path(mod.tree.size() - 1).c_str()));
    cc_json_string.append("}");

    cc_json_string += ']';

    fs::create_directory(mod.install_dir);
    auto const cc_json_path =
        mod.install_dir / fs::path("compile_commands.json");
    auto cc_json = luamake::File(cc_json_path,
                                 luamake::File::WRITE | luamake::File::CREATE);
    if (!cc_json) {
      ferror_message("Unable to make file %s", cc_json_path.c_str());
      return exit_t::internal_error;
    }
    cc_json.write(cc_json_string.c_str(), cc_json_string.size(), 1);
  }
  return exit_t::ok;
}

static auto run(lua_State *const state) noexcept -> exit_t {
  auto build_res = build(state);
  if (build_res != exit_t::ok) {
    error_message("Occurred during build phase of run");
    return build_res;
  }

  auto run_fn = lua_getglobal(state, "Run");
  if (run_fn == LUA_TNIL) {
    error_message("Function `Run` is undefined in the "
                  "discovered `luamake.lua`." LM_NL
                  "\tSee README in [[github link]] for more info.");
    return exit_t::config_error;
  }

  auto runner_t = lua_getglobal(state, luamake::RUNNER_OBJ);
  switch (runner_t) {
  case LUA_TNIL:
    lua_pop(state, 1);
    luamake::builtins::make_runner_obj(state);
    break;
  case LUA_TTABLE:
    break;
  default:
    ferror_message("`runner` object was defined, but it's type was expected to "
                   "be table, got [%s]",
                   lua_typename(state, lua_type(state, -1)));
    return exit_t::config_error;
  };

  // NOTE: technically causing a double deinit, but this seems to work for
  // requiring all of the modules be built before running the run function
  luamake::threads.deinit();

  if (lua_pcall(state, 1, 0, 0) != LUA_OK) {
    auto const err_message = lua_tolstring(state, -1, nullptr);
    ferror_message("While in the lua vm, Run function" LM_NL "\t[%s]",
                   err_message);
    return exit_t::lua_vm_error;
  }

  return exit_t::ok;
}

static auto test(lua_State *const state) noexcept -> exit_t {
  luamake::builtins::make_builder_obj(state);
  lua_pushboolean(state, true);
  lua_setfield(state, -2, luamake::TESTING_MACRO);

  auto const build_res = build(state);
  if (build_res != exit_t::ok) {
    error_message("Occurred during build phase of test");
    return build_res;
  }

  // TODO
  return exit_t::internal_error;
}
} // namespace

auto main(int argc, char **argv) -> int {
  auto const command = determine_command(argc, argv);
  switch (run_command(command, argc, argv)) {
  case exit_t::ok:
    return 0;
  case exit_t::internal_error:
    error_message("Internal Service Error, probably not implimented yet :)");
    return 1;
  case exit_t::lua_vm_error:
    return 1;
  case exit_t::config_error:
    return 70;
  case exit_t::useage_error:
    return 65;
  }
}
