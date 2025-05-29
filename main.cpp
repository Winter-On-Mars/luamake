#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>
#include <type_traits>
#include <vector>

extern "C" {
#include "lua/lauxlib.h"
#include "lua/lua.h"
}

#include "common.hpp"
#include "dependency_graph.hpp"
#include "luamake_builtins.hpp"

using std::array, std::pair, std::make_pair, std::vector, std::string,
    std::string_view;

using u8 = std::uint8_t;

#define BUILDER_OBJ "__luamake_builder"
#define RUNNER_OBJ "__luamake_runner"
#define TESTING_MACRO "__define_testing_macro"

namespace MakeTypes {
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

struct user_func_config final {
  lua_State *state;
  bool release;
};

static auto build(user_func_config const *const) noexcept -> exit_t;
static auto new_proj(char const *, proj_t const) noexcept -> exit_t;
static auto init_proj(char const *, proj_t const) noexcept -> exit_t;
static auto clean() noexcept -> exit_t;
static auto test(user_func_config const *const) noexcept -> exit_t;
static auto run(user_func_config const *const) noexcept -> exit_t;
static auto help() noexcept -> exit_t;

struct Type final {
  enum {
    UNKNOWN_ARG,
    BUILD,
    NEW,
    INIT,
    CLEAN,
    TEST,
    RUN,
    HELP,
  } type_t;

  int argc;
  char **argv;

  static auto make(int, char **) noexcept -> Type;

  auto run() const noexcept -> exit_t;
};

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
  } else if (strcmp(argv[1], "run") == 0) {
    return {Type::RUN, argc, argv};
  } else if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "help") == 0) {
    return {Type::HELP, 0, nullptr};
  } else if (strcmp(argv[1], "i") == 0 || strcmp(argv[1], "init") == 0) {
    return {Type::INIT, argc, argv};
  } else {
    fwarning_message("Unknown argument [%s]" NL
                     "\tDisplaying help for list of accepted arguments",
                     argv[1]);
    return {Type::UNKNOWN_ARG, 0, nullptr};
  }
}

auto Type::run() const noexcept -> exit_t {
  fn_print();
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
    return new_proj(argv[2], project_type);
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
  case CLEAN:
    return clean();
  case HELP:
    return help();
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

  auto *lm_lua = fopen((fs::current_path() / "luamake.lua").c_str(), "r");
  if (lm_lua == nullptr) {
    ferror_message("unable to discover `luamake.lua` in current dir at [%s]" NL
                   "\tRun "
                   "init <proj-name> to create a initialize a new project, "
                   "or new <proj-name> to create a new subproject.",
                   fs::current_path().c_str());
    return exit_t::config_error;
  }
  (void)fclose(lm_lua);
  if (luaL_dofile(state, "luamake.lua") != LUA_OK) {
    ferror_message("unable to run the discovered `luamake.lua` file at "
                   "[%s]" NL "\tLua error message [%s]",
                   fs::current_path().c_str(), lua_tostring(state, -1));
    return exit_t::config_error;
  }

  (void)lua_gc(state, LUA_GCSTOP);

  auto res = exit_t::ok;
  auto cfg = user_func_config{
      state,
      false,
  };

  for (int i = 0; i < argc; ++i) {
    if (strcmp(argv[i], "-r") == 0) {
      cfg.release = true;
    }
  }

  switch (type_t) {
  case BUILD: {
    res = build(&cfg);
  } break;
  case TEST: {
    res = test(&cfg);
  } break;
  case RUN: {
    res = MakeTypes::run(&cfg);
  } break;
  case UNKNOWN_ARG:
    [[fallthrough]];
  case NEW:
    [[fallthrough]];
  case INIT:
    [[fallthrough]];
  case CLEAN:
    [[fallthrough]];
  case HELP:
    res = exit_t::internal_error;
    break;
  }
  lua_close(state);
  return res;
}

static auto build(user_func_config const *const c) noexcept -> exit_t {
  using enum exit_t;
  fn_print();

  lua_pushcfunction(c->state, luamake_builtins::clang);
  lua_setglobal(c->state, "Clang");

  auto build_lua_fn = lua_getglobal(c->state, "Build");
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

  auto builder = lua_getglobal(c->state, BUILDER_OBJ);
  switch (builder) {
  case LUA_TNIL:
    lua_pop(c->state, 1);
    luamake_builtins::make_builder_obj(c->state, BUILDER_OBJ);
    break;
  case LUA_TTABLE:
    break;
  default:
    ferror_message("`builder` object was defined, but it's type was expected "
                   "to be table, got [%s]",
                   lua_typename(c->state, lua_type(c->state, -1)));
    return exit_t::config_error;
  }

  if (lua_pcall(c->state, 1, 1, 0) != LUA_OK) {
    auto err_message = lua_tolstring(c->state, -1, nullptr);
    std::cerr << "error message = " << err_message << '\n';
    return exit_t::internal_error; // ?
  }

  (void)lua_gc(c->state, LUA_GCSTOP);

  return exit_t::ok;
}

static auto new_proj(char const *project_name, proj_t const type) noexcept
    -> exit_t {
  fn_print();
  auto const project_root = fs::current_path() / project_name;

  if (fs::exists(project_root)) {
    ferror_message("Project [%s] already exists at [%s]" NL "\tExiting",
                   project_name, project_root.c_str());
    return exit_t::useage_error;
  }

  // create the dir
  fs::create_directory(project_root);

  // create build + src dirs
  fs::create_directory(project_root / "build");
  fs::create_directory(project_root / "src");

  // creating default `luamake.lua`
  auto *luamake_lua = fopen((project_root / "luamake.lua").c_str(), "w");
  if (luamake_lua == nullptr) {
    ferror_message("Unable to open file at [%s]." NL "\tThis could be an issue "
                   "with permissions, or out of space.",
                   (fs::current_path() / "luamake.lua").c_str());
    return exit_t::internal_error;
  }

  auto constexpr lua_f_content = std::array<string_view, 3>{
      // clang-format off
      string_view{"function Build(builder)" NL
                  "    local exe = {" NL
                  "        name = \"a\"," NL
                  "        root = \"src/main.cpp\"," NL
                  "        compiler = Clang({})," NL
                  "        version = \"0.0.1\"," NL
                  "        install_dir = \"build\"," NL
                  "    }" NL
                  NL
                  "    builder.install_exe(exe)" NL
                  "end" NL
                  NL
                  "function Run(runner)" NL
                  "    runner.exe = \"build/a\"" NL
                  "end" NL
                  NL
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
                  "}" NL},
      string_view{"function Build(builder)" NL
                  "    builder.install_dir = \"build\"" NL
                  "    local dlib = {" NL
                  "        root = \"src/dyn.cpp\"," NL
                  "        compiler = Clang({})," NL
                  "        name = \"a\"," NL
                  "        version = \"0.0.1\"," NL
                  "    }" NL
                  "    builder.install_dynamic(dlib)" NL
                  "end" NL
                  },
      string_view{"function Build(builder)" NL
                  "    builder.install_dir = \"build\"" NL
                  "    local slib = {" NL
                  "        root = \"src/static.cpp\"," NL
                  "        compiler = Clang({})," NL
                  "        name = \"a\"," NL
                  "        version = \"0.0.1\"," NL
                  "    }" NL
                  "    builder.install_static(dlib)" NL
                  "end" NL
                  },
      // clang-format on
  };

  auto const actual_string =
      lua_f_content[static_cast<std::underlying_type_t<proj_t>>(type)];

  if (fprintf(luamake_lua, "%s", actual_string.data()) !=
      actual_string.length()) {
    ferror_message("Unable to write full luamake template string into the lua "
                   "file at [%s]",
                   (project_root / "luamake.lua").c_str());
    fclose(luamake_lua);
    return exit_t::internal_error;
  }

  fflush(luamake_lua);

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

  auto const [header_f_name, impl_f_name] =
      file_paths[static_cast<std::underlying_type_t<proj_t>>(type)];

  auto const [header_string, impl_string] =
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

  auto *impl = fopen((project_root / impl_f_name).c_str(), "w");
  if (impl == nullptr) {
    ferror_message("Unable to open file at [%s]." NL "\tThis could be an issue "
                   "with permissions, or out of space.",
                   (project_root / impl_f_name).c_str());
    return exit_t::internal_error;
  }

  if (type != proj_t::Executable &&
      fprintf(header, "%s", header_string.data()) != header_string.length()) {
    ferror_message("Unable to write full hpp file template string at [%s]",
                   (project_root / header_f_name).c_str());
    fclose(luamake_lua);
    fclose(header);
    fclose(impl);
    return exit_t::internal_error;
  }

  if (fprintf(impl, "%s", impl_string.data()) != impl_string.length()) {
    ferror_message("Unable to write full cpp file template string at [%s]",
                   (project_root / impl_f_name).c_str());
    fclose(luamake_lua);
    fclose(header);
    fclose(impl);
    return exit_t::internal_error;
  }

  fclose(luamake_lua);
  if (header != nullptr)
    fclose(header);
  fclose(impl);

  return exit_t::ok;
}

static auto init_proj(char const *root, proj_t const type) noexcept -> exit_t {
  fn_print();
  auto *luamake_file = fopen("./luamake.lua", "w");
  if (luamake_file == nullptr) {
    ferror_message("Unable to open file at [%s]." NL "\tThis could be an issue "
                   "with permissions, or out of space.",
                   (fs::current_path() / "luamake.lua").c_str());
    return exit_t::internal_error;
  }

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

static auto clean() noexcept -> exit_t {
  fn_print();
  // remove everything from ./build
  // where `.` is the dir that luamake is being called from

  auto const clean_path = fs::current_path() / "build";
  using namespace std::string_view_literals;
  printf("Cleaning [%s]\n", clean_path.c_str());

  if (!fs::exists(clean_path)) {
    printf("[%s] does not exist (either previously cleaned, or never "
           "constructed).\nNothing to do exiting\n",
           clean_path.c_str());
    return exit_t::ok;
  }

  // TODO: finish writting this stuff
  for (auto const &dir_ent : fs::recursive_directory_iterator(clean_path)) {
    // maybe a better way to go about thing?
    switch (dir_ent.status().type()) {
    case fs::file_type::not_found:
    case fs::file_type::none:
    case fs::file_type::regular:
    case fs::file_type::block:
    case fs::file_type::directory:
    case fs::file_type::fifo:
    case fs::file_type::character:
    case fs::file_type::socket:
    case fs::file_type::symlink:
    case fs::file_type::unknown:
      break;
    }
    if (dir_ent.is_regular_file()) {
      std::cout << "Removing" << dir_ent << '\n';
      fs::remove(dir_ent);
    }
    // fs::remove(file);
  }

  printf("Done Cleaning\n");

  return exit_t::ok;
}

static auto test(user_func_config const *const c) noexcept -> exit_t {
  fn_print();
  lua_createtable(c->state, 0, 1); // builder object
  lua_pushboolean(c->state, true);
  lua_setfield(c->state, -2, TESTING_MACRO);
  lua_setglobal(c->state, BUILDER_OBJ);

  auto const build_res = build(c);
  if (build_res != exit_t::ok) {
    error_message("Occurred during build phase of test");
    return build_res;
  }

  // TODO
  return exit_t::internal_error;
}

static auto run(user_func_config const *const c) noexcept -> exit_t {
  fn_print();

  auto build_res = build(c);
  if (build_res != exit_t::ok) {
    error_message("Occurred during build phase of run");
    return build_res;
  }

  auto run_fn = lua_getglobal(c->state, "Run");
  if (run_fn == LUA_TNIL) {
    error_message("Function `Run` is undefined in the "
                  "discovered `luamake.lua`." NL
                  "\tSee README in [[github link]] for more info.");
    return exit_t::config_error;
  }

  // TODO: create `config` object, then call run_fn, and return if there's any
  // errors
  return exit_t::internal_error;
}

static auto help() noexcept -> exit_t {
  fn_print();
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
} // namespace MakeTypes

auto main(int argc, char **argv) -> int {
  auto const flags = MakeTypes::Type::make(argc, argv);
  switch (flags.run()) {
  case MakeTypes::exit_t::ok:
    return 0;
  case MakeTypes::exit_t::internal_error:
    error_message("Internal Service Error, probably not implimented yet :)");
    return 1;
  case MakeTypes::exit_t::lua_vm_error:
    return 1;
  case MakeTypes::exit_t::config_error:
    return 70;
  case MakeTypes::exit_t::useage_error:
    return 65;
  }
}
