#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <lua.hpp>

using std::vector, std::string, std::string_view;

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
  string name;
  std::size_t hash;
  bool recompile;
  vector<src_file> deps;

  auto constexpr notify_deps() noexcept -> void;

  auto constexpr depens_on(string_view const src) const noexcept -> bool {
    if (name == src) {
      return true;
    }
    if (std::any_of(deps.begin(), deps.end(),
                    [src](auto &&dep) { return dep.name == src; })) {
      return true;
    }
    return false;
  }
};

namespace MakeTypes {
auto build(lua_State *) noexcept -> int;
auto new_project(char const *) noexcept -> int;
auto clean() noexcept -> int;
auto test(lua_State *) noexcept -> int;
auto run(lua_State *) noexcept -> int;
auto help() noexcept -> int;

struct Type final {
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
      // TODO: write error message about improperly formatted input
      fprintf(stderr, "");
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
    return {Type::UNKNOWN_ARG, ""};
  }
}

auto Type::run() const noexcept -> int {
  switch (type_t) {
  case UNKNOWN_ARG:
    return help();
  case BUILD: {
    auto *state = luaL_newstate();
    if (state == nullptr) {
      fprintf(stderr,
              "Unable to init lua vm.\nidk what the error is, try adding a "
              "post onto the github with more info so we can fix this issue");
      return 67;
    }
    return build(state);
  }
  case NEW:
    return new_project(project_name);
  case CLEAN:
    return clean();
  case TEST: {
    auto *state = luaL_newstate();
    if (state == nullptr) {
      fprintf(stderr,
              "Unable to init lua vm.\nidk what the error is, try adding a "
              "post onto the github with more info so we can fix this issue");
      return 67;
    }
    return test(state);
  }
  case RUN: {
    auto *state = luaL_newstate();
    if (state == nullptr) {
      fprintf(stderr,
              "Unable to init lua vm.\nidk what the error is, try adding a "
              "post onto the github with more info so we can fix this issue");
      return 67;
    }
    return ::MakeTypes::run(state);
  }
  case HELP:
    return help();
  }
}

auto build(lua_State *) noexcept -> int {
  printf("%s\n", __PRETTY_FUNCTION__);
  help();
  return -1;
}

auto new_project(char const *project_name) noexcept -> int {
  namespace fs = std::filesystem;

  auto const project_root = fs::current_path() / project_name;

  if (fs::exists(project_root)) {
    fprintf(stderr, "Project %s, already exists at %s", project_name,
            project_root.c_str());
    return 1;
  }

  // create the dir
  fs::create_directory(project_root);

  // create build + src dirs
  fs::create_directory(project_root / "build");
  fs::create_directory(project_root / "src");

  // creating default `luamake.lua`
  auto *luamake_lua = fopen((project_root / "luamake.lua").c_str(), "w");
  // TODO: check that this doesn't throw some lifetime error
  // TODO: write the default config for `luamake.lua`
  auto constexpr lua_f_content =
      string_view{"#include <print>\n"
                  "\n"
                  "auto main(int argc, char** argv) -> int {\n"
                  "    std::println(\"Hello World!\");\n"
                  "}"};
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
  auto constexpr cpp_f_content =
      string_view{"#include <print>\n"
                  "\n"
                  "auto main(int argc, char** argv) -> int {\n"
                  "    std::println(\"Hello World!\");\n"
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

  printf("Done Cleaning");

  return 0;
}

auto test(lua_State *state) noexcept -> int {
  // build object
  // TODO: have nrec != 0, figure out how much we'll need
  lua_createtable(state, 0, 0);

  auto const build_res = build(state);
  if (build_res != 0) {
    fprintf(stderr, "Error dunig compiling phase of test");
    return build_res;
  }

  // TODO
}

auto run(lua_State *state) noexcept -> int {
  auto build_res = build(state);
  if (build_res != 0) {
    fprintf(stderr, "Error during build phase of compiling");
    return build_res;
  }

  auto run_fn = lua_getglobal(state, "Run");
  if (lua_type(state, run_fn) == LUA_TNIL) {
    fprintf(stderr,
            "Function `Run` is undefined in the discovered `luamake.lua`. See "
            "README in [[github link]] for more info.");
    return 1;
  }

  // TODO: create `config` object, then call run_fn, and return if there's any
  // errors
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
