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
// TODO: optimize this struct
// potentially remove name, switch to a struct of arrays
// remove the style tag
// pack archive strings into single buffer
// non-lazily grab the needed headers
struct Package final {
  string_view name;
  fs::path headers; // relative paths to the headers + archive
  fs::path archive;
  enum Style { c, cpp } style;
};

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

// TODO: do things with builder.deps for dependency management
// and do things with builder.type to actually fuckin compile
// static and dynamic libs
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
  case LUA_TNIL:          // builder is undefined
    lua_pop(c->state, 1); // remove the nil from the stack otherwise things fuck
                          // up when we call the Build function
    lua_createtable(c->state, 0, 0);
    lua_setglobal(c->state, BUILDER_OBJ);
    lua_getglobal(
        c->state,
        BUILDER_OBJ); // there's definately a better way to go about this, but
                      // i can't think of one rn, basically it's because the
                      // setglobal function pops the value from the stack, but
                      // we need it on the stack bc we're passing it into the
                      // funciton being called :)
    break;
  case LUA_TTABLE: // table already defined, i.e. this function is being
                   // called from run/test
    break;
  default:
    ferror_message("`builder` object was defined, but it's type was expected "
                   "to be table, got [%s]",
                   lua_typename(c->state, lua_type(c->state, -1)));
    return exit_t::config_error;
  }

  // TODO: switch this to pcall
  lua_call(c->state, 1, 0);
  (void)lua_gc(c->state, LUA_GCSTOP);

  builder = lua_getglobal(c->state, BUILDER_OBJ);
  if (builder != LUA_TTABLE) {
    error_message(" You ended up changing the type of the `builder` "
                  "object that was passed into your `Build` "
                  "function." NL "\tIdk just fuck'in don't do that?");
    return exit_t::config_error;
  }

  auto const builder_obj_pos = lua_absindex(c->state, -1);

  auto pre_exec_t = lua_getfield(c->state, builder_obj_pos, "pre_exec");
  if (pre_exec_t == LUA_TTABLE) {
    // TODO: run the pre_exec stuff
    warning_message("pre_exec table things are not currently implimented, and "
                    "thus will not be run" NL
                    "\tConsider adding this section to "
                    "the gh by opening an issue or however github works idk.");
  }
  lua_pop(c->state, 1); // remove the pre_exec stuff

  auto const output_name_t = lua_getfield(c->state, builder_obj_pos, "name");
  if (output_name_t != LUA_TSTRING) {
    error_message("Expected `build.name` to be of type string");
    return exit_t::config_error;
  }

  auto const *output_name = lua_tostring(c->state, -1);

  auto const builder_root_t = lua_getfield(c->state, builder_obj_pos, "root");
  if (builder_root_t != LUA_TSTRING) {
    error_message("fucked up root path to the main file");
    return exit_t::config_error;
  }

  auto const fpath = fs::path(lua_tostring(c->state, -1));

  // TODO: get the project info passed to dependency_graph::generate, so that
  // you can include packages headers without things blowing up :)
  auto const packages_t = lua_getfield(c->state, builder_obj_pos, "packages");
  auto packages = vector<Package>();
  switch (packages_t) {
  case LUA_TNIL:
    break;
  case LUA_TTABLE: {
    auto const pkg_idx = lua_absindex(c->state, -1);
    packages.reserve(lua_rawlen(c->state, pkg_idx));

    lua_pushnil(c->state);
    while (lua_next(c->state, pkg_idx) != 0) {
      if (lua_type(c->state, -2) != LUA_TSTRING) {
        error_message(
            "Expected key type of builder.packages to be of type string");
        return config_error;
      }
      if (lua_type(c->state, -1) != LUA_TTABLE) {
        error_message("Expected builder.packages to be a table of tables");
        return config_error;
      }

      auto const name = string_view{lua_tolstring(c->state, -2, nullptr)};

      auto const header_t = lua_getfield(c->state, -1, "headers");
      if (header_t != LUA_TSTRING) {
        error_message("Package header type incorrect");
        return config_error;
      }
      auto const header_path = fs::path(lua_tolstring(c->state, -1, nullptr));

      auto const header_style_t = lua_getfield(c->state, -2, "header_style");
      if (header_style_t != LUA_TSTRING) {
        error_message("Package header_style type incorrect");
        return config_error;
      }

      auto const header_style = lua_tolstring(c->state, -1, nullptr);
      auto style = Package::c;
      if (strcmp(header_style, "c") == 0) {
        // do nothing
      } else if (strcmp(header_style, "cpp") == 0) {
        style = Package::cpp;
      } else {
        error_message("Package header_style can only be of type 'c' or 'cpp'");
        return config_error;
      }

      auto const lib_t = lua_getfield(c->state, -3, "lib");
      if (lib_t != LUA_TSTRING) {
        error_message("Package lib type incorrect");
        return config_error;
      }
      auto const archive = fs::path(lua_tolstring(c->state, -1, nullptr));

      packages.emplace_back(name, header_path, archive, style);

      lua_pop(c->state, 4); /* remove value, need key for next iteration call */
    }
  } break;
  default:
    error_message("Found builder.packages, but was not of type table");
    return exit_t::config_error;
  }

  for (auto const &package : packages) {
    std::cout << '{' << '\n';
    std::cout << "\tname: " << package.name << '\n';
    std::cout << "\theaders: " << package.headers << '\n';
    std::cout << "\theader_style: "
              << (package.style == Package::c ? "c" : "cpp") << '\n';
    std::cout << "\tarchive: " << package.archive << '\n';
    std::cout << '}' << '\n';
  }

  auto needed_includes = [&packages]() -> vector<string> {
    auto res = vector<string>();
    for (auto &&[_, h_dir, _, style] : packages) {
    }
    return res;
  }();

  auto *root_file = fopen(fpath.c_str(), "r");
  if (root_file == nullptr) {
    ferror_message("The file specified as the root of the project [%s] does "
                   "not exist at [%s]",
                   fpath.c_str(), (fs::current_path() / fpath).c_str());
    return exit_t::config_error;
  }

  // TODO: check that there's no cycles in the dep graph
  auto dep_graph = dependency_graph::generate(root_file, fpath);
  fclose(root_file);

  if (!dep_graph.has_value()) {
    error_message("Generating dependency graph");
    return exit_t::internal_error;
  }

  std::cout << "dep_graph size = " << dep_graph->size() << '\n';
  for (auto const &[k, v] : dep_graph.value()) {
    std::cout << "\t(" << k << ", [";
    for (auto const &str : v) {
      std::cout << str.name << ", ";
    }
    std::cout << "])\n";
  }
  std::cout << std::flush;

  auto const builder_compiler_t =
      lua_getfield(c->state, builder_obj_pos, "compiler");
  auto const compiler_obj_pos = lua_absindex(c->state, -1);
  if (builder_compiler_t != LUA_TTABLE) {
    error_message("Expected `builder.compiler` field to be of type table");
    return exit_t::config_error;
  }

  auto envoked_command = string();
  auto const compiler_field_name_t =
      lua_getfield(c->state, compiler_obj_pos, "compiler");
  if (compiler_field_name_t != LUA_TSTRING) {
    error_message(
        "Expected `builder.compiler.compiler` field to be of type string.");
    return exit_t::config_error;
  }
  auto const *compiler = lua_tostring(c->state, -1);

  auto const compiler_opt_level_t =
      lua_getfield(c->state, compiler_obj_pos, "optimize");
  if (compiler_opt_level_t != LUA_TSTRING) {
    error_message("Expected `builder.compiler.optimize` to be of type string.");
    return exit_t::config_error;
  }
  auto const *compiler_opt_level =
      c->release ? "O3" : lua_tostring(c->state, -1);

  auto const compiler_warnings_t =
      lua_getfield(c->state, compiler_obj_pos, "warnings");
  if (compiler_warnings_t != LUA_TTABLE) {
    error_message(
        "Expected `builder.compiler.warnings` to be of type array (table).");
    return exit_t::config_error;
  }

  auto const warnings_len = lua_rawlen(c->state, -1);
  auto cc_flags = string();
  for (auto [i, warnings_tbl] = make_pair(lua_Unsigned{1}, int{-1});
       i <= warnings_len; ++i, --warnings_tbl) {
    cc_flags += '-';
    auto const warnings_i_t =
        lua_geti(c->state, warnings_tbl, static_cast<lua_Integer>(i));
    if (warnings_i_t != LUA_TSTRING) {
      error_message("TODO: expected string found something else.");
      return exit_t::config_error;
    }
    cc_flags += lua_tostring(c->state, -1);
    cc_flags += ' ';
  }

  for (auto const &[_, dependers] : dep_graph.value()) {
    for (auto const &dep : dependers) {
      if (!dep.is_cpp_file()) {
        continue;
      }
      envoked_command += compiler;
      envoked_command += ' ';

      envoked_command += '-';
      envoked_command += compiler_opt_level;
      envoked_command += ' ';

      envoked_command += cc_flags;

      envoked_command += "-o build/";
      envoked_command += dep.name.stem();
      envoked_command += ".o ";

      envoked_command += "-c ";
      envoked_command += dep.name;

      expr_dbg(envoked_command);
      if (0 != system(envoked_command.c_str())) {
        error_message("When compiling the above command");
        return exit_t::internal_error;
      }
      envoked_command.clear();
    }
  }

  envoked_command += compiler;
  envoked_command += ' ';

  envoked_command += '-';
  envoked_command += compiler_opt_level;
  envoked_command += ' ';

  envoked_command += cc_flags;

  for (auto const &f : fs::directory_iterator(fs::current_path() / "build")) {
    if (fs::relative(f).extension() == fs::path(".o")) {
      envoked_command += fs::relative(f);
      envoked_command += ' ';
    }
  }

  envoked_command += "-o build/";
  // TODO: switch on the type to change the name of the output executable and
  // it's file extension
  envoked_command += output_name;

  expr_dbg(envoked_command);

  if (0 != system(envoked_command.c_str())) {
    error_message("When compiling the output binary");
    return exit_t::internal_error;
  }

  // TODO: post_exec
  auto post_exec_t = lua_getfield(c->state, builder_obj_pos, "post_exec");
  if (post_exec_t == LUA_TTABLE) {
    warning_message(
        "`builder.post_exec` was found, but support is currently not "
        "implimented" NL
        "\tConsider adding support for it by opening an issue on the GH");
  }
  lua_pop(c->state, 1);

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

  // TODO: error checking on making these dirs :)

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
                  "    builder.type = \"exe\"" NL
                  "    builder.root = \"src/main.cpp\"" NL
                  "    builder.compiler = Clang({})" NL
                  "    builder.name = \"a\"" NL
                  NL
                  "    builder.version = \"0.0.1\"" NL
                  "    builder.description = \"TODO change me :)\"" NL
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
                  "    builder.type = \"dlib\"" NL
                  "    builder.root = \"src/dyn.cpp\"" NL
                  "    builder.compiler = Clang({})" NL
                  "    builder.name = \"a\"" NL
                  NL
                  "    builder.version = \"0.0.1\"" NL
                  "    builder.description = \"TODO change me :)\"" NL
                  "end" NL
                  },
      string_view{"function Build(builder)" NL
                  "    builder.type = \"slib\"" NL
                  "    builder.root = \"src/static.cpp\"" NL
                  "    builder.compiler = Clang({})" NL
                  "    builder.name = \"a\"" NL
                  NL
                  "    builder.version = \"0.0.1\"" NL
                  "    builder.description = \"TODO change me :)\"" NL
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
