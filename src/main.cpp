#include "common.hpp"
#include "luamake_builtins.hpp"
#include "luamake_file.hpp"
#include "luamake_thread_pool.hpp"

#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <numeric>
#include <string_view>
#include <thread>
#include <type_traits>

#ifdef __unix__
#include <fcntl.h>
#include <unistd.h>
#endif

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace fs = std::filesystem;

using std::array, std::pair, std::string, std::string_view;

#define BUILDER_OBJ "__luamake_builder"
#define RUNNER_OBJ "__luamake_runner"
#define TESTING_MACRO "__define_testing_macro"

namespace luamake {
namespace {
enum class Value_t { NUMBER, STRING, BOOL_TRUE, BOOL_FALSE, NIL };
auto constexpr determine_type(string_view const str) noexcept -> Value_t {
  // TODO: handle \" (and other escape characters) appearing in the string
  if (str.length() == 0 || str == string_view{"nil"}) {
    return Value_t::NIL; // i don't know if this is actually what we want to do?
  } else if (str == string_view{"true"}) {
    return Value_t::BOOL_TRUE;
  } else if (str == string_view{"false"}) {
    return Value_t::BOOL_FALSE;
  } else if (isalpha(str[0])) {
    return Value_t::STRING;
  } else if (isdigit(str[0])) {
    return Value_t::NUMBER;
  }
  return Value_t::NIL;
}

auto create_args(lua_State *state, int argc, char **argv) noexcept -> void {
  lua_createtable(state, 0, 0);
  auto start_lua_args = 0;
  for (; start_lua_args < argc; ++start_lua_args) {
    if (argv[start_lua_args][0] == '-' && strlen(argv[start_lua_args]) == 2 &&
        argv[start_lua_args][1] == '-') {
      ++start_lua_args;
      break;
    }
  }

  // TODO: finish this, idk add a function that checks if a table value already
  // exists, and appends to it if it does, otherwise creates the value; that way
  // the function can be used to allow us to put subtables into the args table
  for (; start_lua_args < argc; ++start_lua_args) {
    // TODO: parse the args and put them in the table
    auto arg = string(argv[start_lua_args]);
    auto const eq_pos = arg.find('=');
    if (eq_pos == arg.npos) {
      // idk should report an error/warning here?
      // all args passed to the script should be of the form
      // <arg_name> = <literal value>
      // where <literal value> is a lua literal
      continue;
    }
    // NOTE: we have to set this bc lua_setfield just takes in a c_str, and
    // (probably) looks for a '\0'
    arg[eq_pos] = '\0';
    auto const arg_name = std::string_view(arg.data(), arg.data() + eq_pos);
    auto const value_str =
        std::string_view(arg.data() + eq_pos + 1, arg.data() + arg.size());
    switch (determine_type(value_str)) {
    case Value_t::NUMBER: {
      // the function handles both integers and floats depending on lua lex
      // rules
      auto val = lua_stringtonumber(state, value_str.data());
      if (val == 0) {
        // idk report an error
        lua_pushnil(state);
      }
    } break;
    case Value_t::STRING:
      lua_pushlstring(state, value_str.data(), value_str.length());
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
    lua_setfield(state, -2, arg_name.data());
  }
  lua_setglobal(state, "args");
}

enum class exit_t : unsigned char {
  ok,
  internal_error,
  lua_vm_error,
  config_error,
  useage_error,
};

enum class proj_t : unsigned char {
  Executable,
  Dynamic,
  Static,
};

auto file_exists(fs::path &&path) noexcept -> bool {
  // TODO: rewrite this because O_PATH is linux specific, see man 2 open for
  // info
#if defined(__unix__)
  auto file = open(path.c_str(), O_PATH);
  close(file);
  return file != -1;
#elif __cplusplus >= 201703L
  try {
    return fs::exists(path);
  } catch (...) {
    return false;
  }
#else
  auto *file = fopen(path.c_str(), "r");
  fclose(file);
  return file != nullptr;
#endif
}

static auto new_proj(string_view const, proj_t const) noexcept -> exit_t;
static auto init_proj(string_view const, proj_t const) noexcept -> exit_t;
static auto help() noexcept -> exit_t;

static auto build(lua_State *const) noexcept -> exit_t;
static auto clean(lua_State *const) noexcept -> exit_t;
static auto compile_commands_json(lua_State *const) noexcept -> exit_t;
static auto run(lua_State *const) noexcept -> exit_t;
static auto test(lua_State *const) noexcept -> exit_t;

struct Type final {
  // TODO: add command for generating compile_commands.json to the project
  // add optional argument for running in verbose mode to output more
  // information like the specific thread things are being run on
  enum class Command : int {
    UNKNOWN_ARG,
    BUILD,
    NEW,
    INIT,
    CLEAN,
    TEST,
    RUN,
    HELP,
    CC_JSON,
  } type_t;
  using enum Command;

  int argc;
  char **argv;

  static auto make(int, char **) noexcept -> Type;

  auto do_command() const noexcept -> exit_t;
};

// TODO: just pass argc and argv to the functions directly, there's no reason to
// be attaching them to the objects like this ?
auto Type::make(int argc, char **argv) noexcept -> Type {
  if (argc == 1) {
    return {Type::RUN, 0, nullptr};
  }

  if (strcmp(argv[1], "b") == 0 || strcmp(argv[1], "build") == 0) {
    return {Type::BUILD, argc, argv};
  } else if (strcmp(argv[1], "n") == 0 || strcmp(argv[1], "new") == 0) {
    if (argc < 3) {
      error_message("Expected string for the name of the "
                    "project, found nothing" NL
                    "\tDisplaying help message for more information" NL);
      return {Type::HELP, 0, nullptr};
    }
    return {Type::NEW, argc, argv};
  } else if (strcmp(argv[1], "c") == 0 || strcmp(argv[1], "clean") == 0) {
    return {Type::CLEAN, 0, nullptr};
  } else if (strcmp(argv[1], "t") == 0 || strcmp(argv[1], "test") == 0) {
    return {Type::TEST, argc, argv};
  } else if (strcmp(argv[1], "r") == 0 || strcmp(argv[1], "run") == 0) {
    return {Type::RUN, argc, argv};
  } else if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "help") == 0) {
    return {Type::HELP, 0, nullptr};
  } else if (strcmp(argv[1], "i") == 0 || strcmp(argv[1], "init") == 0) {
    return {Type::INIT, argc, argv};
  } else if (strcmp(argv[1], "cc") == 0 ||
             strcmp(argv[1], "compile_commands") == 0) {
    return {Type::CC_JSON, 0, nullptr};
  } else {
    fwarning_message("Unknown argument [%s]" NL
                     "\tDisplaying help for list of accepted arguments",
                     argv[1]);
    return {Type::UNKNOWN_ARG, 0, nullptr};
  }
}

auto Type::do_command() const noexcept -> exit_t {
  switch (type_t) {
  case UNKNOWN_ARG:
    return help();
  case NEW: {
    auto project_type = proj_t::Executable;
    for (int i = 0; i < argc; ++i) {
      if (strcmp("--static", argv[i]) == 0) {
        project_type = proj_t::Static;
        break;
      } else if (strcmp("--executable", argv[i]) == 0) {
        break;
      } else if (strcmp("--dynamic", argv[i]) == 0) {
        project_type = proj_t::Dynamic;
        break;
      }
    }
    return new_proj(string_view{argv[2]}, project_type);
  }
  case INIT: {
    char const *project_root = nullptr;
    auto project_type = proj_t::Executable;

    for (int i = 0; i < argc; ++i) {
      if (*argv[i] != '-') {
        continue;
      }
      auto const len = strlen(argv[i]);
      if (len < 5) {
        continue;
      }

      if (strncmp(argv[i], "-type", 4) == 0) {
        ++i;
        if (strcmp(argv[i], "executable") == 0) {
        } else if (strcmp(argv[i], "dynamic") == 0) {
          project_type = proj_t::Dynamic;
        } else if (strcmp(argv[i], "static") == 0) {
          project_type = proj_t::Static;
        } else {
          ferror_message(
              "When parsing for 'init' command line args, came across "
              "unknown type %s",
              argv[i]);
          return exit_t::useage_error;
        }
      } else if (strncmp(argv[i], "-root", 4) == 0) {
        ++i;
        project_root = argv[i];
      }
    }
    if (project_root == nullptr) {
      error_message(
          "When parsing for 'init' command line args, expected a project root "
          "file to be given, see help message for more information");
      return exit_t::useage_error;
    }
    return init_proj(project_root, project_type);
  }
  case HELP:
    return help();
  case CLEAN:
    [[fallthrough]];
  case CC_JSON:
    [[fallthrough]];
  case BUILD:
    [[fallthrough]];
  case TEST:
    [[fallthrough]];
  case RUN:
    break;
  }

  auto *state = luaL_newstate();
  if (state == nullptr) {
    error_message(
        "Unable to init luavm." NL
        "\tThere may be some issue with your lua lib, if "
        "not feel free to message me on discord/ open an issue on the "
        "gh");
    return exit_t::lua_vm_error; // internal service error
  }
  (void)lua_gc(state, LUA_GCSTOP);

  if (!file_exists(fs::current_path() / "luamake.lua")) {
    ferror_message("unable to discover `luamake.lua` in current dir at [%s]" NL
                   "\tRun "
                   "init <proj-name> to create a initialize a new project, "
                   "or new <proj-name> to create a new subproject.",
                   fs::current_path().c_str());
    return exit_t::config_error;
  }

  luaL_openlibs(state);
  lua_register(state, "Dump", luamake::builtins::dump);

  create_args(state, argc, argv);

  if (luaL_dofile(state, "luamake.lua") != LUA_OK) {
    ferror_message("unable to run the discovered `luamake.lua` file at "
                   "[%s]" NL "\tLua error message [%s]",
                   fs::current_path().c_str(), lua_tostring(state, -1));
    return exit_t::config_error;
  }

  (void)lua_gc(state, LUA_GCSTOP);

  auto res = exit_t::ok;

  for (int i = 0; i < argc; ++i) {
    if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
      builtins::cl_options.verbose = true;
    }
  }

  builtins::mods.init();
  // this can arguably be moved into just the build function, because that's the
  // only one that really needs a thread pool, but for now we'll do it here
  threads.init(std::thread::hardware_concurrency() - 1);
  switch (type_t) {
  case BUILD:
    res = build(state);
    break;
  case TEST:
    res = test(state);
    break;
  case RUN:
    res = run(state);
    break;
  case CLEAN:
    res = clean(state);
    break;
  case CC_JSON:
    res = compile_commands_json(state);
    break;
  case UNKNOWN_ARG:
    [[fallthrough]];
  case NEW:
    [[fallthrough]];
  case INIT:
    [[fallthrough]];
  case HELP:
    unreachable();
  }
  threads.deinit();
  builtins::mods.deinit();
  lua_close(state);
  return res;
}

static auto new_proj(string_view const project_name, proj_t const type) noexcept
    -> exit_t {
  auto const project_root = fs::current_path() / project_name;

  if (fs::exists(project_root)) {
    ferror_message("Project [%s] already exists at [%s]" NL "\tExiting",
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
      File(project_root / "luamake.lua", File::WRITE | File::CREATE);
  if (!luamake_lua) {
    ferror_message("Unable to open file at [%s]." NL "\tThis could be an issue "
                   "with permissions, or out of space.",
                   (fs::current_path() / "luamake.lua").c_str());
    return exit_t::internal_error;
  }

  // these are all format strings, so they need to be passed to std::format
  auto constexpr lua_f_content = std::array<string_view, 3>{
      // clang-format off
      string_view{"function Build(b)" NL
                  "    local exe = b:new_exe({{" NL
                  "        name = \"{0}\"," NL
                  "        root = \"src/main.cpp\"," NL
                  "        compiler = b.clang({{}})," NL
                  "        version = \"0.0.1\"," NL
                  "        install_dir = \"build\"," NL
                  "    }})" NL
                  NL
                  "    return b.install_exe(exe)" NL
                  "end" NL
                  NL
                  "function Run(r)" NL
                  "    local exe = {{" NL
                  "        name = \"{0}\"," NL
                  "        path = \"build/{0}\"," NL
                  "        args = {{}}," NL
                  "    }}" NL
                  NL
                  "    r.run(exe)" NL
                  "end" NL
                  NL
                  "Tests = {{" NL
                  "    {{" NL
                  "        fun = function(t)" NL
                  "            t.exe = \"build/{0}\"" NL
                  "            t.args = {{\"This does nothing\"}}" NL
                  "        end," NL
                  "        output = {{" NL
                  "            expected = \"Hello World!\\n\"," NL
                  "            from = \"stdout\"," NL
                  "        }}," NL
                  "    }}" NL
                  "}}" NL},
      string_view{"local function Build(b)" NL
                  "    local dlib = b:new_dynamic({{" NL
                  "        roots = {{ \"src/dyn.cpp\" }}," NL
                  "        headers = {{ \"src/dyn.hpp\" }}," NL
                  "        compiler = b.clang({{}})," NL
                  "        name = \"{0}\"," NL
                  "        version = \"0.0.1\"," NL
                  "        install_dir = \"build\"," NL
                  "    }})" NL
                  "    return b.install_dynamic(dlib)" NL
                  "end" NL
                  NL
                  "return {{" NL
                  "    Build = Build" NL
                  "}}"
                  },
      string_view{"local function Build(b)" NL
                  "    local slib = b:new_static({{" NL
                  "        roots = {{ \"src/static.cpp\" }}," NL
                  "        headers = {{ \"src/static.hpp\" }}," NL
                  "        compiler = b.clang({{}})," NL
                  "        name = \"{0}\"," NL
                  "        version = \"0.0.1\"," NL
                  "        install_dir = \"build\"," NL
                  "    }})" NL
                  "    return b.install_static(slib)" NL
                  "end" NL
                  NL
                  "return {{" NL
                  "    Build = Build" NL
                  "}}"
                  },
      // clang-format on
  };

  auto const actual_string = std::vformat(
      lua_f_content[static_cast<std::underlying_type_t<proj_t>>(type)],
      std::make_format_args(project_name));

  if (luamake_lua.write(actual_string.c_str(), actual_string.size(), 1) !=
      actual_string.size()) {
    ferror_message("Unable to write full luamake template string into the lua "
                   "file at [%s]",
                   (project_root / "luamake.lua").c_str());
    return exit_t::internal_error;
  }

  luamake_lua.flush();

  auto constexpr file_paths = array<pair<string_view, string_view>, 3>{
      pair("", "src/main.cpp"),
      pair("src/dyn.hpp", "src/dyn.cpp"),
      pair("src/static.hpp", "src/static.cpp"),
  };

  auto constexpr hpp_cpp_f_content = array<pair<string_view, string_view>, 3>{
      pair(string_view{""},
           string_view{
               ""
               // clang-format off
               "#include <iostream>" NL
               NL
               "auto main() -> int {" NL
               "    using std::cout;" NL
               "    cout << \"Hello World!\" << std::endl;" NL
               "}" NL
               // clang-format on
           }),
      pair(
          string_view{
              ""
              // clang-format off
              "#pragma once" NL
              NL
              "namespace dlib {" NL
              "[[nodiscard]]" NL
              "auto call_me(int) noexcept -> int;" NL
              "}" NL
              // clang-format on
          },
          string_view{
              ""
              // clang-format off
              "#include \"dyn.hpp\"" NL
              NL
              "namespace dlib {" NL
              "[[nodiscard]]" NL
              "auto call_me(int i) noexcept -> int {" NL
              "    return i + 1;" NL
              "}" NL
              "}" NL
              // clang-format on
          }),
      pair(
          string_view{
              ""
              // clang-format off
              "#pragma once" NL
              NL
              "namespace slib {" NL
              "[[nodiscard]]" NL
              "auto call_me(int) noexcept -> int;" NL
              "}" NL
              // clang-format on
          },
          string_view{
              ""
              // clang-format off
              "#include \"static.hpp\"" NL
              NL
              "namespace slib {" NL
              "[[nodiscard]]" NL
              "auto call_me(int i) noexcept -> int {" NL
              "    return i + 1;" NL
              "}" NL
              "}" NL
              // clang-format on
          }),
  };

  auto &&[header_f_name, impl_f_name] =
      file_paths[static_cast<std::underlying_type_t<proj_t>>(type)];

  auto &&[header_string, impl_string] =
      hpp_cpp_f_content[static_cast<std::underlying_type_t<proj_t>>(type)];

  auto *header = (!header_f_name.empty())
                     ? fopen((project_root / header_f_name).c_str(), "w")
                     : nullptr;
  if (type != proj_t::Executable && header == nullptr) {
    ferror_message("Unable to open file at [%s]." NL "\tThis could be an issue "
                   "with permissions, or out of space.",
                   (project_root / header_f_name).c_str());
    return exit_t::internal_error;
  }

  auto impl = File(project_root / impl_f_name, File::WRITE | File::CREATE);
  if (!impl) {
    ferror_message("Unable to open file at [%s]." NL "\tThis could be an issue "
                   "with permissions, or out of space.",
                   (project_root / impl_f_name).c_str());
    return exit_t::internal_error;
  }

  if (type != proj_t::Executable &&
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

// TODO: update this function so that the template strings are actually correct
static auto init_proj(string_view root, proj_t const type) noexcept -> exit_t {
  auto *luamake_file = fopen("./luamake.lua", "w");
  if (luamake_file == nullptr) {
    ferror_message("Unable to open file at [%s]." NL "\tThis could be an issue "
                   "with permissions, or out of space.",
                   (fs::current_path() / "luamake.lua").c_str());
    return exit_t::internal_error;
  }

  // TODO: update this to use std::format
  auto luamake_content = string();
  luamake_content.reserve(256);

  // TODO: rework this to have the separated objects that are constructed
  auto constexpr types = std::array<string_view, 3>{
      string_view{"exe"},
      string_view{"dlib"},
      string_view{"slib"},
  };

  auto const project_type_string =
      types[static_cast<std::underlying_type_t<proj_t>>(type)];

  luamake_content +=
      // clang-format off
    string_view{
      "function Build(builder)" NL
           "    builder.type = \""
    };
  // clang-format on
  luamake_content += project_type_string;
  luamake_content +=
      // clang-format off
    string_view{"\"" NL
        "    builder.root = \""
    };
  // clang-format on
  luamake_content += root;
  luamake_content +=
      // clang-format off
    string_view{"\"" NL
        "    builder.compiler = Clang({})" NL
        "    builder.name = \"a\"" NL // TODO: let user when calling this function specify the output name
        NL
        "    builder.version = \"0.0.1\"" NL
        "    builder.description = \"TODO change me :)\"" NL
        "end" NL
    };
  // clang-format on

  if (type == proj_t::Executable) {
    luamake_content +=
        // clang-format off
    string_view{""
      "function Run(runner)" NL
      "    runner.exe = \"build/a\"" NL
      "end" NL
      };
    // clang-format on
    luamake_content +=
        // clang-format off
    string_view{""
      "Tests = {" NL
      "    {" NL
      "        fun = function(tester)" NL
      "            tester.exe = \"build/a\"" NL
      "            tester.args = {\"This does nothing\"}" NL
      "        end," NL
      "        output = {" NL
      "            expected = \"Hello World!\\n\"," NL
      "            from = \"stdout\"," NL
      "        }," NL
      "    }" NL
      "}" NL
    };
    // clang-format on
  }

  // idk if it's worth calling this function?
  // [luamake_content.shrink_to_fit()]
  if (fprintf(luamake_file, "%s", luamake_content.data()) !=
      luamake_content.length()) {
    ferror_message(
        "Unable to write `luamake.lua` content into luamake file at [%s]",
        fs::current_path().c_str());
    fclose(luamake_file);
    fs::remove("./luamake.lua"); // delete file for attomic rw
    return exit_t::internal_error;
  }

  fclose(luamake_file);

  return exit_t::ok;
}

static auto help() noexcept -> exit_t {
  // clang-format off
  printf(
      "Usage: luamake [options]?" NL
      "options:" NL
      "\t-h, help                          : Displays this help message." NL
      "\tc, clean                          : Cleans the cache dir and removes the output." NL
      "\tn, new <project-name>             : Creates a new subdir with name <project-name>, "
      "creating a default luamake build script." NL
      "\ti, init <project-name> [init-args]:" NL
      "\tb, build                          : Builds the project based on the `Build` function "
      "defined in the `luamake.lua` file in the current dir." NL
      "\tt, test                           : Builds the project based on the `Build` function "
      "in the `luamake.lua` file in the current dir, with the additional macro "
      "`LUAMAKE_TESTS` defined. Then runs the tests defined in the `Test` "
      "function "
      "defined in the `luamake.lua` file in the current dir, displaying the "
      "number of tests that succeeded." NL
      "\tr, run                            : Builds the project based on the `Build` function "
      "defined in the `luamake.lua` file in the current dir. Then runs the "
      "program, based on the `Run` function defined in the current dirs "
      "`luamake.lua` file." NL
      "If no options are passed in, it is the same as calling `luamake -r`" NL
      "For more information see the `README.md` at "
      "[[https://github.com/Winter-On-Mars/luamake]]" NL);
  // clang-format on
  fflush(stdout);
  return exit_t::ok;
}

static auto build(lua_State *const state) noexcept -> exit_t {
  auto const build_lua_fn = lua_getglobal(state, "Build");
  // function undefined in `luamake.lua`
  if (build_lua_fn == LUA_TNIL) {
    error_message(
        "Unable to find function `Build` in discovered `luamake.lua`." NL
        "\tSee README/wiki for more info");
    return exit_t::config_error;
  }
  // value Build is defined as a global, but isn't a function
  if (build_lua_fn != LUA_TFUNCTION) {
    error_message("`Build` value found in `luamake.lua`, but is not a "
                  "function (might be callable [why would you do that?])." NL
                  "\tSee README/wiki for more info, and if is a callable, feel "
                  "free to open a gh issue to fix this problem (and maybe "
                  "explain why the code's formatted this way lol)");
    return exit_t::config_error;
  }

  auto const builder = lua_getglobal(state, BUILDER_OBJ);
  switch (builder) {
  case LUA_TNIL:
    lua_pop(state, 1);
    builtins::make_builder_obj(state);
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
    ferror_message("While in the lua vm, Build function" NL "\t[%s]",
                   err_message);
    return exit_t::lua_vm_error; // ?
  }

  return exit_t::ok;
}

// TODO: add a command line arg to specify which directories you want to clean,
// something like --base for the root directory, along with being able to
// specify the name of a specific module to clean
// NOTE: this function just clears the cache, it leaves every other file as is,
// we should add a command line arg to fully remove the files, something like
// --everything
static auto clean(lua_State *const state) noexcept -> exit_t {
  auto const build_fn_t = lua_getglobal(state, "Build");
  switch (build_fn_t) {
  case LUA_TFUNCTION:
    break;
  case LUA_TNIL:
    error_message(
        "Unable to find function `Build` in discovered `luamake.lua`." NL
        "\tSee README/wiki for more info");
    return exit_t::config_error;
  default:
    error_message("`Build` value found in `luamake.lua`, but is not a "
                  "function (might be callable [why would you do that?])." NL
                  "\tSee README/wiki for more info, and if is a callable, feel "
                  "free to open a gh issue to fix this problem (and maybe "
                  "explain why the code's formatted this way lol)");
    return exit_t::config_error;
  }

  // normally we need to get the builder object from the global, but in this
  // case there's no other point that can call this function, so we just need to
  // make a builder object
  builtins::make_builder_thunk(state);
  if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
    auto const err_message = lua_tolstring(state, -1, nullptr);
    ferror_message("While in the lua vm, Build function" NL "\t[%s]",
                   err_message);
    return exit_t::lua_vm_error; // ?
  }

  for (auto &&mod : builtins::mods) {
    auto const cache_path = fs::path(
        std::format("{}/__luamake_cache/{}.cache", mod.install_dir, mod.name));
    (void)fs::remove(cache_path);
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
        "Unable to find function `Build` in discovered `luamake.lua`." NL
        "\tSee README/wiki for more info");
    return exit_t::config_error;
  default:
    error_message("`Build` value found in `luamake.lua`, but is not a "
                  "function (might be callable [why would you do that?])." NL
                  "\tSee README/wiki for more info, and if is a callable, feel "
                  "free to open a gh issue to fix this problem (and maybe "
                  "explain why the code's formatted this way lol)");
    return exit_t::config_error;
  }

  // normally we need to get the builder object from the global, but in this
  // case there's no other point that can call this function, so we just need to
  // make a builder object
  builtins::make_builder_thunk(state);
  if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
    auto const err_message = lua_tolstring(state, -1, nullptr);
    ferror_message("While in the lua vm, Build function" NL "\t[%s]",
                   err_message);
    return exit_t::lua_vm_error; // ?
  }

  for (auto &&mod : builtins::mods) {
    auto const &directory = mod.install_dir;
    auto const arguments = [&]() -> string {
      auto res = string();
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
    auto const dep_includes =
        std::accumulate(mod.dep_includes.cbegin(), mod.dep_includes.cend(),
                        std::string(), [](auto &&a, auto &&next) {
                          return std::format("{}\"-iquote\",\"{}\",", a,
                                             next.parent_path().string());
                        });

    auto cc_json_string = std::string(1, '[');
    for (auto i = size_t{}; i < mod.tree.size() - 1; ++i) {
      cc_json_string.append("{");
      cc_json_string.append(std::format("\"directory\":\"{}\",", directory));

      cc_json_string.append("\"arguments\":[");
      cc_json_string.append(arguments);

      cc_json_string.append(includes);
      cc_json_string.append(sys_includes);
      cc_json_string.append(dep_includes);

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
    cc_json_string.append(dep_includes);

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
    auto cc_json = File(cc_json_path, File::WRITE | File::CREATE);
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
                  "discovered `luamake.lua`." NL
                  "\tSee README in [[github link]] for more info.");
    return exit_t::config_error;
  }

  auto runner_t = lua_getglobal(state, RUNNER_OBJ);
  switch (runner_t) {
  case LUA_TNIL:
    lua_pop(state, 1);
    builtins::make_runner_obj(state);
    break;
  case LUA_TTABLE:
    break;
  default:
    ferror_message("`runner` object was defined, but it's type was expected to "
                   "be table, got [%s]",
                   lua_typename(state, lua_type(state, -1)));
    return exit_t::config_error;
  };

  if (lua_pcall(state, 1, 0, 0) != LUA_OK) {
    auto const err_message = lua_tolstring(state, -1, nullptr);
    ferror_message("While in the lua vm, Run function" NL "\t[%s]",
                   err_message);
    return exit_t::lua_vm_error;
  }

  return exit_t::ok;
}

static auto test(lua_State *const state) noexcept -> exit_t {
  builtins::make_builder_obj(state);
  lua_pushboolean(state, true);
  lua_setfield(state, -2, TESTING_MACRO);

  auto const build_res = build(state);
  if (build_res != exit_t::ok) {
    error_message("Occurred during build phase of test");
    return build_res;
  }

  // TODO
  return exit_t::internal_error;
}
} // namespace
} // namespace luamake

auto main(int argc, char **argv) -> int {
  using namespace luamake;
  auto const flags = Type::Type::make(argc, argv);
  switch (flags.do_command()) {
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
