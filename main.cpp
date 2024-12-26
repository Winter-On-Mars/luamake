#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifdef __linux
#include <sysexits.h>
#endif

#include <lua.hpp>

// TODO: really just a lot of things to do with cross platform support
// mostly just with the \r\n vs \n, and the terminal colors listed below

// color things for error messages/ warnings
// TODO: add an option to not define these in the `Makefile`
// TODO: extract these into a platform independent thing so this'll actually
// work on windows and shit
#ifdef NO_TERM_COLOR
#define ERROR
#define WARNING
#define DBG
#define NORMAL
#else
#define ERROR "\033[0;31m"
#define WARNING "\033[0;33m"
#define DBG "\033[0;32m"
#define NORMAL "\033[0;0m"
#endif

using std::vector, std::string, std::string_view;

// TODO: remove this and add <utility> header if you ever upgrade to c++23
// source = [[https://en.cppreference.com/w/cpp/utility/unreachable]]
[[noreturn]] void unreachable() {
#if defined(_MSC_VER) && !defined(__clang__)
  __assume(false);
#else
  __builtin_unreachable();
#endif
}

// TODO: update all fprintf(stderr) to have the ERROR thing so the terminal
// output is colored

#define BUILDER_OBJ "__luamake_builder"
#define RUNNER_OBJ "__luamake_runner"
#define TESTING_MACRO "__define_testing_macro"

namespace luamake_builtins {
static auto clang(lua_State* state) -> int {
  // TODO: actually impl this function
  auto num_args = lua_gettop(state);
  if (num_args != 1) {
    lua_pushliteral(state, "fuck");
  } else {
    lua_createtable(state, 0, 0);
  }
  return 1;
}

static luaL_Reg const funcs[] = {
  { "Clang", clang },
  { nullptr, nullptr }
};
}

namespace helper {
auto constexpr flatten(std::span<string_view const> const arr,
                       string_view const seperator) noexcept -> string {
  auto const sep = string(seperator);
  return std::accumulate(
      arr.begin(), arr.end(), string(),
      [sep](auto &&init, auto &&n) { return (init + sep) + string(n); });
}

// how tf do you do cpp doc comments >:(
/* @description: dumps content of lua value in standard way */
auto constexpr dump(lua_State *state /*, lua_table* ...*/) noexcept -> string {
  // TODO: actually write function
  return string("");
}
} // namespace helper

namespace dependency_graph {
// all of the strings we parse from the config.lua are owned by
// the lua_State gc, so these all are non-owning references
struct Config final {
  string_view compiler;
  vector<string_view> flags;
  string_view name;
  string_view type;
  vector<string_view> linking;
  string_view linking_dir;
  vector<string_view> pre_exec;
  vector<string_view> post_exec;

  static auto make(lua_State *) noexcept -> std::optional<Config>;

  friend auto operator<<(std::ostream &, Config const &) noexcept
      -> std::ostream &;
};

// See what functions modify the lua_State stack
auto Config::make(lua_State *state) noexcept -> std::optional<Config> {
  // returns the type of the global as an int
  // all of the lua_get... functions seem to just return the type of the
  // returned value (could be useful)
  (void)lua_getglobal(state, "config_table");
  int cfg_tbl_pos_in_stack = -1;

  (void)lua_getfield(state, cfg_tbl_pos_in_stack, "compiler");
  --cfg_tbl_pos_in_stack;
  auto *compiler_str = luaL_tolstring(state, -1, NULL);
  // luaL_tolstring pushes a value onto the stack for some reason :(
  --cfg_tbl_pos_in_stack;

  (void)lua_getfield(state, cfg_tbl_pos_in_stack, "bin_name");
  --cfg_tbl_pos_in_stack;
  auto *bin_name_str = luaL_tolstring(state, -1, NULL);
  --cfg_tbl_pos_in_stack;

  // checking to see what happens if we try to get a field that doesn't exist
  (void)lua_getfield(state, cfg_tbl_pos_in_stack, "output_type");
  --cfg_tbl_pos_in_stack;
  // pretty sure we could just use lua_isnil(state, -1)
  auto output_type =
      (lua_type(state, -1) == LUA_TNIL)
          ? (std::cout
                 << "No type found: Defaulting to binary output (If this isn't "
                    "what you want read the README for more information)\n",
             std::string_view{"binary"})
          : (--cfg_tbl_pos_in_stack,
             std::string_view{luaL_tolstring(state, -1, NULL)});

  auto flags = vector<string_view>();
  flags.reserve(4);
  (void)lua_getfield(state, cfg_tbl_pos_in_stack, "flags");
  if (lua_isnil(state, -1)) {
    std::cout << "Error: config.flags == nil\n";
    std::exit(64);
  }
  if (!lua_istable(state, -1)) {
    std::cout << "Error: typeof(config.flags) ~= table\n";
    std::exit(64);
  }
  int flags_pos_in_stack = -1;
  --cfg_tbl_pos_in_stack;

  auto const len = lua_rawlen(state, flags_pos_in_stack);
  for (auto i = 1ull; i <= len; ++i) {
    (void)lua_rawgeti(state, flags_pos_in_stack, static_cast<lua_Integer>(i));
    --flags_pos_in_stack;
    --cfg_tbl_pos_in_stack;
    auto tmp = string_view{luaL_tolstring(state, -1, NULL)};
    flags.push_back(tmp);
  }

  return Config{/* compiler= */ string_view{compiler_str},
                /* flags= */ flags,
                /* name (out_name)= */ string_view{bin_name_str},
                /* type= */ output_type,
                /* linking= */ vector<string_view>(),
                /* linking_dir= */ string_view{},
                /* pre_exec= */ vector<string_view>(),
                /* post_exec= */ vector<string_view>()};
}

auto operator<<(std::ostream &os, Config const &cfg) noexcept
    -> std::ostream & {
  // don't like that we have to cast the std::string_view to a std::string,
  // because it seems like there should be string::operator+(string_view)
  // but whatever :)
  auto const flags =
      std::accumulate(cfg.flags.cbegin(), cfg.flags.cend(), std::string(),
                      [](auto &&init, auto &&next) {
                        return (init + ", ") + std::string(next);
                      });
  auto const linking =
      cfg.linking.size() == 0
          ? std::string("nil")
          : std::accumulate(cfg.linking.cbegin(), cfg.linking.cend(),
                            std::string(), [](auto &&init, auto &&next) {
                              return init + ", " + std::string(next);
                            });

  auto const pre_exec =
      cfg.pre_exec.size() == 0
          ? std::string("nil")
          : std::accumulate(cfg.pre_exec.cbegin(), cfg.pre_exec.cend(),
                            std::string(), [](auto &&init, auto &&next) {
                              return init + ", " + std::string(next);
                            });
  auto const post_exec =
      cfg.post_exec.size() == 0
          ? std::string("nil")
          : std::accumulate(cfg.post_exec.cbegin(), cfg.post_exec.cend(),
                            std::string(), [](auto &&init, auto &&next) {
                              return init + ", " + std::string(next);
                            });

  os << "{\n\tcompiler: " << cfg.compiler;
  os << ",\n\tflags: " << flags;
  os << ",\n\tbin_name: " << cfg.name;
  os << ",\n\ttype: " << cfg.type;
  os << ",\n\tlinking: " << linking;
  os << ",\n\tlinking_dir: " << cfg.linking_dir;
  os << ",\n\tpre_exec: " << pre_exec;
  os << ",\n\tpost_exec: " << post_exec;

  os << "\n}\n";

  return os;
}

struct src_file final {
  std::size_t hash;
  string name;
  vector<string> deps;
};

using dep_graph = std::unordered_map<string, src_file>;

auto generate(FILE* root, char const *root_name) noexcept -> dep_graph {
  auto graph = dep_graph{};

  char * line = nullptr;
  try {
    char* line = nullptr;
    size_t len = 0;
    size_t amount_read = getline(&line, &len, root);
    // todo: finish this stupid fuckin c code shit
  } catch (...) {
  }

  free(line);

  return graph;
}
}

namespace MakeTypes {
auto build(lua_State *) noexcept -> int;
auto new_project(char const *) noexcept -> int;
auto clean() noexcept -> int;
auto test(lua_State *) noexcept -> int;
auto run(lua_State *) noexcept -> int;
auto help() noexcept -> int;

struct Type final {
  // TODO: add init flag to inplace init a new project
  // TODO: add --release flag for O3 optimizations
  enum {
    UNKNOWN_ARG,
    BUILD,
    NEW,
    CLEAN,
    TEST,
    RUN,
    HELP,
  } type_t;

  char const *project_name;

  static auto make(int, char **) noexcept -> Type;

  auto run() const noexcept -> int;
};

auto Type::make(int argc, char **argv) noexcept -> Type {
  if (argc == 1) {
    return {Type::RUN, ""};
  }

  if (strcmp(argv[1], "-b") == 0) {
    return {Type::BUILD, ""};
  } else if (strcmp(argv[1], "-n") == 0) {
    // TODO: make sure to see there's no potential security
    // issues with this command
    if (argc != 3) {
      fprintf(stderr, ERROR
              "Fatel Error:" NORMAL " Expected string for the name of the "
              "project, found nothing"
              "\n\tDisplaying help message for more information\n");
      return {Type::HELP, ""};
    }
    return {Type::NEW, argv[2]};
  } else if (strcmp(argv[1], "-c") == 0) {
    return {Type::CLEAN, ""};
  } else if (strcmp(argv[1], "-t") == 0) {
    return {Type::TEST, ""};
  } else if (strcmp(argv[1], "-r") == 0) {
    return {Type::RUN, ""};
  } else if (strcmp(argv[1], "-h") == 0) {
    return {Type::HELP, ""};
  } else {
    printf(WARNING "Unknown argument" NORMAL " [%s]\n"
                   "\tDisplaying help for list of accepted arguments\n",
           argv[1]);
    return {Type::UNKNOWN_ARG, ""};
  }
}

auto Type::run() const noexcept -> int {
  switch (type_t) {
  case UNKNOWN_ARG:
    return help();
  case NEW:
    return new_project(project_name);
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

  // TODO: actually close down the lua_State*
  // when we're done
  auto *state = luaL_newstate();
  if (state == nullptr) {
    fprintf(stderr, ERROR
            "Fatel Error:" NORMAL " Unable to init luavm.\n"
            "\tThere may be some issue with your lua lib, if "
            "not feel free to message me on discord/ open an issue on the "
            "gh");
    return 70; // internal service error
  }

  // TODO: remove this and make it a bit safer
  luaL_openlibs(state);

  auto* lm_lua = fopen((std::filesystem::current_path()/"luamake.lua").c_str(), "r");
  if (lm_lua == nullptr) {
    fprintf(stderr, ERROR "Fatel Error:" NORMAL " unable to discover `luamake.lua` in current dir at [%s]\n\tRun -n <proj-name> to create a new project [and maybe in the future i'll add a -i to init a new project we'll see :)]", std::filesystem::current_path().c_str());
    return EX_CONFIG;
  }
  (void)fclose(lm_lua);
  if (luaL_dofile(state, "luamake.lua")) {
    fprintf(stderr, ERROR "Fatel Error:" NORMAL " unable to run the discovered `luamake.lua` file at [%s]\n\tLua error message [%s]", std::filesystem::current_path().c_str(), lua_tostring(state, -1));
    return EX_CONFIG;
  }

  switch (type_t) {
  case BUILD:
    return build(state);
  case TEST:
    return test(state);
  case RUN:
    return MakeTypes::run(state);
  case UNKNOWN_ARG:
    [[fallthrough]];
  case NEW:
    [[fallthrough]];
  case CLEAN:
    [[fallthrough]];
  case HELP:
    break;
  }
  unreachable();
}

auto build(lua_State *state) noexcept -> int {
  printf(DBG "fn" NORMAL " [%s]\n", __PRETTY_FUNCTION__);
  fflush(stdout);

  lua_pushcfunction(state, luamake_builtins::clang);
  lua_setglobal(state, "Clang");

  auto build_lua_fn = lua_getglobal(state, "Build");
  // function undefined in `luamake.lua`
  if (build_lua_fn == LUA_TNIL) {
    fprintf(stderr, ERROR
            "Fatel Error:" NORMAL
            " Unable to find function `Build` in discovered `luamake.lua`.\n"
            "\tSee README/wiki for more info\n");
    return 70;
  }
  // value Build is defined as a global, but isn't a function
  if (build_lua_fn != LUA_TFUNCTION) {
    fprintf(stderr,
            ERROR "Fatel Error:" NORMAL
                  " `Build` value found in `luamake.lua`, but is not a "
                  "function (might be callable [why would you do that?]).\n"
                  "\tSee README/wiki for more info, and if is a callable, feel "
                  "free to open a gh issue to fix this problem (and maybe "
                  "explain why the code's formatted this way lol)\n");
    return 70;
  }

  auto builder = lua_getglobal(state, BUILDER_OBJ);
  switch (builder) {
  case LUA_TNIL:       // builder is undefined
    lua_pop(state, 1); // remove the nil from the stack otherwise things fuck up
                       // when we call the Build function
    lua_createtable(state, 0, 0);
    lua_setglobal(state, BUILDER_OBJ);
    lua_getglobal(state, BUILDER_OBJ); // there's definately a better way to go about this, but i can't think of one rn, basically it's because the setglobal function pops the value from the stack, but we need it on the stack bc we're passing it into the funciton being called :)
    break;
  case LUA_TTABLE: // table already defined, i.e. this function is being called
                   // from run/test
    break;
  default:
      // TODO: check that this is the correct use of lua_typename
      fprintf(stderr, ERROR "Fatel Error:" NORMAL " `builder` object was defined, but it's type was expected to be table, got [%s]", lua_typename(state, 1));
    return -1;
  }

  // TODO: switch this to pcall
  lua_call(state, 1, 0);

  // TODO: pre_exec
  builder = lua_getglobal(state, BUILDER_OBJ);
  if (builder != LUA_TTABLE) {
    fprintf(stderr, ERROR "Fatel Error:" NORMAL " You ended up changing the type of the `builder` object that was passed into your `Build` function.\n\tIdk just fuck'in don't do that?\n");
    return 70;
  }

  auto pre_exec_t = lua_getfield(state, -1, "pre_exec");
  if (pre_exec_t == LUA_TTABLE) {
    // TODO: run the pre_exec stuff
    fprintf(stderr, WARNING "Warning:" NORMAL " pre_exec table things are not currently implimented, and thus will not be run\n\tConsider adding this section to the gh by opening an issue or however github works idk.");
  }
  lua_pop(state, 1); // remove the pre_exec stuff

  auto const builder_root_t = lua_getfield(state, -1, "root");
  if (builder_root_t != LUA_TSTRING) {
    fprintf(stderr, ERROR "Fatel Error:" NORMAL " fucked up root path to the main file");
    return 70;
  }

  auto const* builder_root = lua_tostring(state, -1);
  printf("Testing builder_root [%s]\n", builder_root);
  fflush(stdout);

  auto* root_file = fopen((std::filesystem::current_path()/builder_root).c_str(), "r");
  if (root_file == nullptr) {
    fprintf(stderr, ERROR "Fatel Error:" NORMAL " The file specified as the root of the project [%s] does not exist at [%s]", builder_root, (std::filesystem::current_path()/builder_root).c_str());
    return -1;
  }

  // TODO: strip the 'src/' from the builder_root
  auto const* stripped_builder_name = builder_root;
  auto dep_graph = dependency_graph::generate(root_file, stripped_builder_name);

  fclose(root_file);

  // TODO: post_exec

  return -1;
}

auto new_project(char const *project_name) noexcept -> int {
  namespace fs = std::filesystem;

  auto const project_root = fs::current_path() / project_name;

  if (fs::exists(project_root)) {
    fprintf(stderr,
            ERROR "Fatel Error: Project" NORMAL " [%s] " ERROR
                  " already exists at" NORMAL " [%s]\n"
                  "\tExiting\n",
            project_name, project_root.c_str());
    return 1;
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
    // TODO: error message about not being able to open file
    return -1;
  }
  // TODO: check that this doesn't throw some lifetime error
  auto constexpr lua_f_content =
      string_view{"function Build(builder)\n"
                  "    builder.type = \"exe\"\n"
                  "    builder.root = \"src/main.cpp\"\n"
                  "    builder.compiler = Clang({})\n"
                  "    builder.name = \"a\"\n"
                  "\n"
                  "    builder.version = \"0.0.1\"\n"
                  "    builder.description = \"TODO change me :)\"\n"
                  "end\n"
                  "\n"
                  "function Run(runner)\n"
                  "    runner.exe = \"build/a\"\n"
                  "end\n"
                  "\n"
                  "Tests = {\n"
                  "    {\n"
                  "        fun = function(tester)\n"
                  "            tester.exe = \"build/a\"\n"
                  "            tester:add_args({\"This does nothing\"})\n"
                  "        end,\n"
                  "        expected_output = \"Hello World!\\n\"\n"
                  "    }\n"
                  "}\n"};
  auto amount_lua_written = fprintf(luamake_lua, "%s", lua_f_content.data());
  if (amount_lua_written != lua_f_content.length()) {
    fprintf(
        stderr,
        "Unable to write full luamake template string into the lua file at %s",
        (project_root / "luamake.lua").c_str());
    fclose(luamake_lua);
    return 65;
  }

  // creating simple hello world cpp file
  auto main_cpp = fopen((project_root / "src/main.cpp").c_str(), "w");
  if (main_cpp == nullptr) {
    // TODO: error message about not being able to open file
    return -1;
  }
  auto constexpr cpp_f_content =
      string_view{"#include <iostream>\n"
                  "\n"
                  "auto main(int argc, char** argv) -> int {\n"
                  "    using std::cout;\n"
                  "    cout << \"Hello World!\\n\";\n"
                  "}\n"};
  auto amount_cpp_written = fprintf(main_cpp, "%s", cpp_f_content.data());
  if (amount_cpp_written != cpp_f_content.length()) {
    fprintf(stderr,
            "Unable to write full c++ template string into the c++ file at %s",
            (project_root / "src/main.cpp").c_str());
    fclose(luamake_lua);
    fclose(main_cpp);
    return 65;
  }

  fclose(luamake_lua);
  fclose(main_cpp);

  return 0;
}

auto clean() noexcept -> int {
  namespace fs = std::filesystem;
  // remove everything from ./build
  // where `.` is the dir that luamake is being called from

  auto const clean_path = fs::current_path() / "build";
  using namespace std::string_view_literals;
  printf("Cleaning [%s]\n", clean_path.c_str());

  if (!fs::exists(clean_path)) {
    printf("[%s] does not exist (either previously cleaned, or never "
           "constructed).\nNothing to do exiting\n",
           clean_path.c_str());
    return 0;
  }

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

  return 0;
}

auto test(lua_State *state) noexcept -> int {
  // builder object
  lua_createtable(state, 0, 0);
  lua_pushboolean(state, true);
  lua_setfield(state, -2, TESTING_MACRO);
  lua_setglobal(state, BUILDER_OBJ);

  auto const build_res = build(state);
  if (build_res != 0) {
    fprintf(stderr, "Error dunig compiling phase of test");
    return build_res;
  }

  // TODO
  return -1;
}

auto run(lua_State *state) noexcept -> int {
  printf(DBG "fn" NORMAL " [%s]\n", __PRETTY_FUNCTION__);
  fflush(stdout);

  auto build_res = build(state);
  if (build_res != 0) {
    fprintf(stderr, "Error during build phase of compiling");
    return build_res;
  }

  auto run_fn = lua_getglobal(state, "Run");
  if (run_fn == LUA_TNIL) {
    fprintf(stderr,
            ERROR "Fatel Error:" NORMAL " Function `Run` is undefined in the "
                  "discovered `luamake.lua`.\n"
                  "\tSee README in [[github link]] for more info.");
    return 70;
  }

  // TODO: create `config` object, then call run_fn, and return if there's any
  // errors
  return -1;
}

auto help() noexcept -> int {
  std::printf(
      "Usage: luamake [options]?\n"
      "options:\n"
      "\t-h               : Displays this help message.\n"
      "\t-c               : Cleans the cache dir and removes the output.\n"
      "\t-n <project-name>: Creates a new subdir with name <project-name>, "
      "creating a default luamake build script.\n"
      "\t-b               : Builds the project based on the `Build` function "
      "defined in the `luamake.lua` file in the current dir.\n"
      "\t-t               : Builds the project based on the `Build` function "
      "in the `luamake.lua` file in the current dir, with the additional macro "
      "`LUAMAKE_TESTS` defined. Then runs the tests defined in the `Test` "
      "function "
      "defined in the `luamake.lua` file in the current dir, displaying the "
      "number of tests that succeeded.\n"
      "\t-r               : Builds the project based on the `Build` function "
      "defined in the `luamake.lua` file in the current dir. Then runs the "
      "program, based on the `Run` function defined in the current dirs "
      "`luamake.lua` file.\n"
      "If no options are passed in, it is the same as calling `luamake -r`\n"
      "For more information see the `README.md` at "
      "[[https://github.com/Winter-On-Mars/luamake]]\n");
  return 1;
}
} // namespace MakeTypes

/*
auto constexpr get_all_files(MakeFlags const &flags) noexcept
    -> std::pair<std::unordered_map<string, src_file>, cache> {
  auto files = std::unordered_map<string, src_file>();
  for (auto const &files : std::filesystem::open_dir(flags.build_dir)) {
    std::cout << files.name << '\n';
  }

  if (!has_cache_dir(flags.build_dir)) {
  }
}
*/

auto main(int argc, char **argv) -> int {
  auto const flags = MakeTypes::Type::make(argc, argv);
  return flags.run();
}
