#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <lua.hpp>

#if defined(_WIN32)
#define NL "\r\n"
#elif defined(__unix__)
#define NL "\n"
#elif defined(__MACH__)
#define NL "\n"
#else
#warning("new line macro defined, you can help the project by adding another header guard and defining it");
#define NL ""
#endif

// color things for error messages/ warnings
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

#define error_message(msg)                                                     \
  fprintf(stderr, ERROR "Fatel Error:" NORMAL " " msg NL)
#define ferror_message(msg, ...)                                               \
  fprintf(stderr, ERROR "Fatel Error:" NORMAL " " msg NL, __VA_ARGS__)
#define warning_message(msg)                                                   \
  fprintf(stderr, WARNING "Warning:" NORMAL " " msg NL)
#define fwarning_message(msg, ...)                                             \
  fprintf(stderr, WARNING "Warning:" NORMAL " " msg NL, __VA_ARGS__)

#define fn_print()                                                             \
  fprintf(stderr, "\t" DBG "calling" NORMAL " [%s]" NL, __PRETTY_FUNCTION__)

#define expr_dbg(expr)                                                         \
  do {                                                                         \
    auto const res = (expr);                                                   \
    std::cerr << DBG "[expr] " NORMAL #expr " = " << res << '\n';              \
  } while (false);

using std::array, std::vector, std::string, std::string_view;

namespace fs = std::filesystem;

template <class T> using opt = std::optional<T>;

// TODO: remove this and add <utility> header if you ever upgrade to c++23
// source = [[https://en.cppreference.com/w/cpp/utility/unreachable]]
[[noreturn]] void unreachable() {
#if defined(_MSC_VER) && !defined(__clang__)
  __assume(false);
#else
  __builtin_unreachable();
#endif
}

#define BUILDER_OBJ "__luamake_builder"
#define RUNNER_OBJ "__luamake_runner"
#define TESTING_MACRO "__define_testing_macro"

namespace luamake_builtins {
static auto clang(lua_State *state) -> int {
  fn_print();

  auto num_args = lua_gettop(state);
  if (num_args != 1) {
    lua_pushnil(state);
    return 1;
  }
  auto constexpr compiler_field = string_view{"clang++"};
  auto constexpr opt_level = string_view{"O2"};
  auto constexpr warnings = array<string_view, 3>{{
      string_view{"Wall"},
      string_view{"Wconversion"},
      string_view{"Wpedantic"},
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

auto dump_impl(lua_State *state, int const idx) noexcept -> string {
  switch (lua_type(state, idx)) {
  case LUA_TNONE:
    lua_error(state);
    return string(); // (?)
  case LUA_TNIL:
    return string("nil");
  case LUA_TBOOLEAN:
    return lua_toboolean(state, idx) == 1 ? string("true") : string("false");
  case LUA_TLIGHTUSERDATA: {
    auto res = string(luaL_tolstring(state, idx, nullptr));
    lua_pop(state, 1);
    return res;
  } break;
  case LUA_TNUMBER:
    return std::to_string(lua_tonumber(state, idx));
  case LUA_TSTRING:
    return string(lua_tostring(state, idx));
  case LUA_TTABLE: {
    auto res = string("Work in progress on dumping tables values\n");
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
    auto res = string("<lua: fn>");
    res += ' ';
    res += luaL_tolstring(state, idx, nullptr);
    lua_pop(state, 1);
    return res;
  } break;
  case LUA_TUSERDATA: {
    auto res = string(luaL_tolstring(state, idx, nullptr));
    lua_pop(state, 1);
    return res;
  } break;
  case LUA_TTHREAD: {
    auto res = string("<lua: thread>");
    res += string(luaL_tolstring(state, idx, nullptr));
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

  auto res = string();

  switch (lua_type(state, -1)) {
  case LUA_TNONE:
    lua_error(state);
    return 1; // (?)
    break;
  case LUA_TNIL:
    res = string("nil");
    break;
  case LUA_TBOOLEAN:
    res = lua_toboolean(state, -1) == 1 ? string("true") : string("false");
    break;
  case LUA_TLIGHTUSERDATA:
    luaL_tolstring(state, -1, nullptr);
    return 1;
    break;
  case LUA_TNUMBER:
    res = std::to_string(lua_tonumber(state, -1));
    break;
  case LUA_TSTRING:
    res = string(lua_tostring(state, -1));
    break;
  case LUA_TTABLE:
    res += string("Work in progress on dumping tables values\n");
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
    res = string("<lua: fn>");
    break;
  case LUA_TUSERDATA:
    luaL_tolstring(state, -1, nullptr);
    return 1;
    break;
  case LUA_TTHREAD:
    res = string("<lua: thread>");
    res += string(luaL_tolstring(state, -1, nullptr));
    lua_pop(state, 1);
    break;
  }

  lua_pushstring(state, res.c_str());
  return 1;
}

constexpr luaL_Reg const funcs[] = {
    {"clang", clang}, {"dump", dump}, {nullptr, nullptr}};
} // namespace luamake_builtins

namespace dependency_graph {
struct src_file final {
  // std::size_t hash;
  string name;
};

using dep_graph = std::unordered_map<string, vector<src_file>>;

[[nodiscard]]
auto skip_ws(char const *current, char const *const end) noexcept
    -> char const * {
  while (current != end) {
    switch (*current) {
    case ' ':
    case '\n':
    case '\t':
    case '\r':
      ++current;
      break;
    default:
      return current;
    }
  }
  return nullptr; // (?)
}

struct LWF final {
  FILE *handle;

  LWF(fs::path const f_name) noexcept : handle(fopen(f_name.c_str(), "r")) {}
  ~LWF() noexcept {
    if (handle != nullptr)
      fclose(handle);
  }
};

auto generate_files_deps(dep_graph &graph, string_view const f_name,
                         fs::path rel_path, char *line) noexcept
    -> void /* return code(?)*/ {
  fn_print();
  auto file = LWF(rel_path / f_name);
  if (file.handle == nullptr) {
    ferror_message(
        "While parsing include directives, unable to open file at %s",
        f_name.data());
    return;
  }

  try {
    size_t len = 0;

    for (auto amount_read = getline(&line, &len, file.handle);
         amount_read != -1; amount_read = getline(&line, &len, file.handle)) {

      auto include_line = string_view(line, len);
      if (include_line.find("#include") == string_view::npos) {
        continue;
      }

      auto const start = include_line.find('"');
      auto const end = include_line.rfind('"');

      if (start == end) {
        continue;
      }

      auto const name_len = end - start - 1; // end includes the '"'
      if (name_len <= string_view{".xpp"}.length()) {
        error_message("While parsing inclued directives, space between the "
                      "includes is not large enough to include a .cpp or .hpp");
        return;
      }

      auto const name = include_line.substr(start + 1, name_len);
      auto const hpp_pos = name.find(".hpp");
      auto const cpp_pos = name.find(".cpp");

      if (hpp_pos == string_view::npos && cpp_pos == string_view::npos) {
        error_message("While parsing include directives, unable to find '.hpp' "
                      "or '.cpp' in files name");
        return;
      }

      if (hpp_pos != string_view::npos && cpp_pos != string_view::npos) {
        ferror_message("While parsing include directives, found multiple valid "
                       "file extensions on line [%s]",
                       line);
        return;
      }

      if (hpp_pos != string_view::npos) {
        auto const last_hpp_pos = name.rfind(".hpp");
        if (last_hpp_pos != hpp_pos) {
          ferror_message(
              "While parsing include directives, found multiple instances of "
              "'.hpp' file extension in line [%s]" NL
              "\tIf this is intentional, maybe as a part of some code "
              "generation, open an issue on gh and we can work to resolve it",
              line);
          return;
        }
        auto const next_f_name = include_line.substr(start, name_len);
        graph[string(f_name)].push_back(src_file(string(next_f_name)));
        generate_files_deps(graph, next_f_name, rel_path, line);
      } else {
        auto const last_cpp_pos = name.rfind(".cpp");
        if (last_cpp_pos != cpp_pos) {
          ferror_message(
              "While parsing include directives, found multiple instances of "
              "'.hpp' file extension in line [%s]" NL
              "\tIf this is intentional, maybe as a part of some code "
              "generation, open an issue on gh and we can work to resolve it",
              line);
          return;
        }
        auto const next_f_name = include_line.substr(start, name_len);
        graph[string(f_name)].push_back(src_file(string(next_f_name)));
        generate_files_deps(graph, next_f_name, rel_path, line);
      }
    }
  } catch (...) {
    ferror_message("Something went wrong while attempting to generate "
                   "dependencies of [%s]",
                   f_name.data());
  }
  free(line);
}

auto generate(FILE *root, string_view const root_name,
              fs::path rel_path) noexcept -> opt<dep_graph> {
  fn_print();
  auto graph = dep_graph{};

  auto *line = static_cast<char *>(malloc(128 * sizeof(char)));
  if (line == nullptr) {
    ferror_message("Unable to reserve %zu, space for line reading buffer",
                   128 * sizeof(char));
    return std::nullopt;
  }

  try {
    size_t len = 0;

    for (auto amount_read = getline(&line, &len, root); amount_read != -1;
         amount_read = getline(&line, &len, root)) {

      auto include_line = string_view(line, len);

      if (include_line.find("#include") == string_view::npos) {
        continue;
      }

      auto const start = include_line.find('"');
      auto const end = include_line.rfind('"');

      if (start == end) {
        continue;
      }

      auto const name_len = end - start - 1; // end includes the '"'
      if (name_len <= string_view{".xpp"}.length()) {
        error_message("While parsing include directives, space between the "
                      "includes is not large enough to include a .cpp or .hpp");
        return std::nullopt;
      }

      auto const name = include_line.substr(start + 1, name_len);
      auto const hpp_pos = name.find(".hpp");
      auto const cpp_pos = name.find(".cpp");
      if (hpp_pos == string_view::npos && cpp_pos == string_view::npos) {
        error_message("While parsing include directives, unable to find .hpp "
                      "or .cpp in the files name");
        return std::nullopt;
      }

      if (hpp_pos != string_view::npos && cpp_pos != string_view::npos) {
        // TODO: remove the NL char from the line buf bc it kinda fucks error
        // messages up :(
        ferror_message("While parsing include directives, found multiple valid "
                       "file extensions on line [%s]",
                       line);
        return std::nullopt;
      }

      if (hpp_pos != string_view::npos) {
        auto const last_hpp_pos = name.rfind(".hpp");
        if (last_hpp_pos != hpp_pos) {
          ferror_message(
              "While parsing include directives, found multiple instances of "
              "'.hpp' file extension in line [%s]" NL
              "\tIf this is intentional, maybe as a part of some code "
              "generation, open an issue on gh and we can work to resolve it",
              line);
          return std::nullopt;
        }
        graph[string(name)].push_back(src_file(string(root_name)));
        generate_files_deps(graph, name, rel_path, line);
      } else {
        auto const last_cpp_pos = name.rfind(".cpp");
        if (last_cpp_pos != cpp_pos) {
          ferror_message(
              "While parsing include directives, found multiple instances of "
              "'.cpp' file extension in line [%s]" NL
              "\tIf this is intentional, maybe as a part of some code "
              "generation, open an issue on gh and we can work to resolve it",
              line);
          return std::nullopt;
        }
        graph[string(name)].push_back(src_file(string(root_name)));
        generate_files_deps(graph, name, rel_path, line);
      }
    }
  } catch (...) {
    error_message("Something went wrong while generating dependencies\n\tAn "
                  "exception was thrown");
  }

  for (auto const &files : fs::directory_iterator(rel_path)) {
    std::cout << "[" << files << "]\n";
  }
  std::cout << std::flush;

  free(line);

  return graph;
}
} // namespace dependency_graph

namespace MakeTypes {
enum class exit_t : unsigned char {
  ok,
  internal_error,
  lua_vm_error,
  config_error,
  useage_error,
};

auto build(lua_State *) noexcept -> exit_t;
auto new_proj(char const *) noexcept -> exit_t;
auto init_proj() noexcept -> exit_t;
auto clean() noexcept -> exit_t;
auto test(lua_State *) noexcept -> exit_t;
auto run(lua_State *) noexcept -> exit_t;
auto help() noexcept -> exit_t;

struct Type final {
  // TODO: add --release flag for O3 optimizations
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

  char const *project_name;

  static auto make(int, char **) noexcept -> Type;

  auto run() const noexcept -> exit_t;
};

auto Type::make(int argc, char **argv) noexcept -> Type {
  if (argc == 1) {
    return {Type::RUN, ""};
  }

  if (strcmp(argv[1], "b") == 0 || strcmp(argv[1], "build") == 0) {
    return {Type::BUILD, ""};
  } else if (strcmp(argv[1], "n") == 0 || strcmp(argv[1], "new") == 0) {
    // TODO: make sure to see there's no potential security
    // issues with this command
    if (argc != 3) {
      error_message("Expected string for the name of the "
                    "project, found nothing" NL
                    "\tDisplaying help message for more information" NL);
      return {Type::HELP, ""};
    }
    return {Type::NEW, argv[2]};
  } else if (strcmp(argv[1], "c") == 0 || strcmp(argv[1], "clean") == 0) {
    return {Type::CLEAN, ""};
  } else if (strcmp(argv[1], "t") == 0 || strcmp(argv[1], "test") == 0) {
    return {Type::TEST, ""};
  } else if (strcmp(argv[1], "run") == 0) {
    return {Type::RUN, ""};
  } else if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "help") == 0) {
    return {Type::HELP, ""};
  } else if (strcmp(argv[1], "init") == 0) {
    return {Type::INIT, ""};
  } else {
    fwarning_message("Unknown argument [%s]" NL
                     "\tDisplaying help for list of accepted arguments",
                     argv[1]);
    return {Type::UNKNOWN_ARG, ""};
  }
}

auto Type::run() const noexcept -> exit_t {
  fn_print();
  switch (type_t) {
  case UNKNOWN_ARG:
    return help();
  case NEW:
    return new_proj(project_name);
  case INIT:
    return init_proj();
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

  // TODO: remove this and make it a bit safer
  luaL_openlibs(state);

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
  switch (type_t) {
  case BUILD:
    res = build(state);
    break;
  case TEST:
    res = test(state);
    break;
  case RUN:
    res = MakeTypes::run(state);
    break;
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

auto build(lua_State *state) noexcept -> exit_t {
  fn_print();

  lua_pushcfunction(state, luamake_builtins::clang);
  lua_setglobal(state, "Clang");

  auto build_lua_fn = lua_getglobal(state, "Build");
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

  auto builder = lua_getglobal(state, BUILDER_OBJ);
  switch (builder) {
  case LUA_TNIL:       // builder is undefined
    lua_pop(state, 1); // remove the nil from the stack otherwise things fuck
                       // up when we call the Build function
    lua_createtable(state, 0, 0);
    lua_setglobal(state, BUILDER_OBJ);
    lua_getglobal(
        state,
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
                   lua_typename(state, lua_type(state, -1)));
    return exit_t::config_error;
  }

  // TODO: switch this to pcall
  lua_call(state, 1, 0);
  (void)lua_gc(state, LUA_GCSTOP);

  // TODO: pre_exec
  builder = lua_getglobal(state, BUILDER_OBJ);
  if (builder != LUA_TTABLE) {
    error_message(" You ended up changing the type of the `builder` "
                  "object that was passed into your `Build` "
                  "function." NL "\tIdk just fuck'in don't do that?");
    return exit_t::config_error;
  }

  auto pre_exec_t = lua_getfield(state, -1, "pre_exec");
  if (pre_exec_t == LUA_TTABLE) {
    // TODO: run the pre_exec stuff
    warning_message("pre_exec table things are not currently implimented, and "
                    "thus will not be run" NL
                    "\tConsider adding this section to "
                    "the gh by opening an issue or however github works idk.");
  }
  lua_pop(state, 1); // remove the pre_exec stuff

  auto const builder_root_t = lua_getfield(state, -1, "root");
  if (builder_root_t != LUA_TSTRING) {
    error_message("fucked up root path to the main file");
    return exit_t::config_error;
  }

  auto const [rel_path, froot] =
      [](lua_State *state) -> std::pair<fs::path, string_view> {
    auto const builder_root = string_view(lua_tostring(state, -1));
    auto const path_split = builder_root.find('/');
    return std::make_pair(fs::path(builder_root.substr(0, path_split)),
                          builder_root.substr(path_split + 1));
  }(state);
  expr_dbg(rel_path);
  expr_dbg(froot);

  auto *root_file = fopen((fs::current_path() / rel_path / froot).c_str(), "r");
  if (root_file == nullptr) {
    ferror_message("The file specified as the root of the project [%s] does "
                   "not exist at [%s]",
                   (rel_path / froot).c_str(),
                   (fs::current_path() / rel_path / froot).c_str());
    return exit_t::config_error;
  }

  // TODO: check that there's no cycles in the dep graph
  auto dep_graph = dependency_graph::generate(root_file, froot, rel_path);
  fclose(root_file);

  if (!dep_graph.has_value()) {
    error_message("fucky wucky :3");
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

  auto const builder_compiler_t = lua_getfield(state, -2, "compiler");
  if (builder_compiler_t != LUA_TTABLE) {
    error_message("Expected `builder.compiler` field to be of type table");
    return exit_t::config_error;
  }

  auto envoked_command = string();
  auto const compiler_field_name_t = lua_getfield(state, -1, "compiler");
  if (compiler_field_name_t != LUA_TSTRING) {
    error_message(
        "Expected `builder.compiler.compiler` field to be of type string.");
    return exit_t::config_error;
  }
  auto const *compiler = lua_tostring(state, -1);
  envoked_command += compiler;
  envoked_command += ' ';

  auto const compiler_opt_level_t = lua_getfield(state, -2, "optimize");
  if (compiler_opt_level_t != LUA_TSTRING) {
    error_message("Expected `builder.compiler.optimize` to be of type string.");
    return exit_t::config_error;
  }
  auto const *compiler_opt_level = lua_tostring(state, -1);
  envoked_command += '-';
  envoked_command += compiler_opt_level;
  envoked_command += ' ';

  auto const compiler_warnings_t = lua_getfield(state, -3, "warnings");
  if (compiler_warnings_t != LUA_TTABLE) {
    error_message(
        "Expected `builder.compiler.warnings` to be of type array (table).");
    return exit_t::config_error;
  }

  auto const warnings_len = lua_rawlen(state, -1);
  auto cc_flags = string();
  for (auto [i, warnings_tbl] = std::make_pair(lua_Unsigned{1}, int{-1});
       i <= warnings_len; ++i, --warnings_tbl) {
    cc_flags += '-';
    auto const warnings_i_t =
        lua_geti(state, warnings_tbl, static_cast<lua_Integer>(i));
    if (warnings_i_t != LUA_TSTRING) {
      error_message("TODO: expected string found something else.");
      return exit_t::config_error;
    }
    cc_flags += lua_tostring(state, -1);
    cc_flags += ' ';
  }

  envoked_command += cc_flags;

  envoked_command += "-o build/";
  envoked_command += froot;
  envoked_command += ".o ";

  envoked_command += "-c ";
  envoked_command += rel_path / froot;

  expr_dbg(envoked_command);

  //  auto sys_res = system(envoked_command.c_str());
  //  if (sys_res != 0) {
  //    // TODO: error
  //    return exit_t::internal_error;
  //  }

  envoked_command.clear();

  envoked_command += compiler;
  envoked_command += ' ';

  envoked_command += '-';
  envoked_command += compiler_opt_level;
  envoked_command += ' ';

  envoked_command += cc_flags;

  auto const output_name_t = lua_getfield(state, -warnings_len - 6, "name");
  if (output_name_t != LUA_TSTRING) {
    error_message("Expected `build.name` to be of type string");
    return exit_t::config_error;
  }

  for (auto const &f : fs::directory_iterator(fs::current_path() / "build")) {
    expr_dbg(fs::relative(f));
    expr_dbg(fs::relative(f).extension());
    if (fs::relative(f).extension() == fs::path(".o")) {
      envoked_command += fs::relative(f);
      envoked_command += ' ';
    }
  }

  auto const output_name = lua_tostring(state, -1);

  envoked_command += "-o build/";
  envoked_command += output_name;

  expr_dbg(envoked_command);

  //  sys_res = system(envoked_command.c_str());
  //  if (sys_res != 0) {
  //    // TODO: error
  //    return exit_t::internal_error;
  //  }

  // TODO: post_exec

  return exit_t::ok;
}

auto new_proj(char const *project_name) noexcept -> exit_t {
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
  auto constexpr lua_f_content =
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
                  "}" NL};
  // clang-format on
  auto amount_lua_written = fprintf(luamake_lua, "%s", lua_f_content.data());
  if (amount_lua_written != lua_f_content.length()) {
    ferror_message("Unable to write full luamake template string into the lua "
                   "file at [%s]",
                   (project_root / "luamake.lua").c_str());
    fclose(luamake_lua);
    return exit_t::internal_error;
  }

  // creating simple hello world cpp file
  auto main_cpp = fopen((project_root / "src/main.cpp").c_str(), "w");
  if (main_cpp == nullptr) {
    ferror_message("Unable to open file at [%s]." NL "\tThis could be an issue "
                   "with permissions, or out of space.",
                   (fs::current_path() / "src/main.cpp").c_str());
    return exit_t::internal_error;
  }
  auto constexpr cpp_f_content =
      // clang-format off
      string_view{"#include <iostream>" NL
                  NL
                  "auto main(int argc, char** argv) -> int {" NL
                  "    using std::cout;" NL
                  "    cout << \"Hello World!\\n\";" NL
                  "}" NL};
  // clang-format on
  auto amount_cpp_written = fprintf(main_cpp, "%s", cpp_f_content.data());
  if (amount_cpp_written != cpp_f_content.length()) {
    ferror_message(
        "Unable to write full c++ template string into the c++ file at [%s]",
        (project_root / "src/main.cpp").c_str());
    fclose(luamake_lua);
    fclose(main_cpp);
    return exit_t::internal_error;
  }

  fclose(luamake_lua);
  fclose(main_cpp);

  return exit_t::ok;
}

auto init_proj() noexcept -> exit_t {
  fn_print();
  return exit_t::internal_error;
}

auto clean() noexcept -> exit_t {
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

auto test(lua_State *state) noexcept -> exit_t {
  fn_print();
  lua_createtable(state, 0, 1); // builder object
  lua_pushboolean(state, true);
  lua_setfield(state, -2, TESTING_MACRO);
  lua_setglobal(state, BUILDER_OBJ);

  auto const build_res = build(state);
  if (build_res != exit_t::ok) {
    error_message("Occurred during build phase of test");
    return build_res;
  }

  // TODO
  return exit_t::internal_error;
}

auto run(lua_State *state) noexcept -> exit_t {
  fn_print();

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

  // TODO: create `config` object, then call run_fn, and return if there's any
  // errors
  return exit_t::internal_error;
}

auto help() noexcept -> exit_t {
  fn_print();
  // clang-format off
  printf(
      "Usage: luamake [options]?" NL
      "options:" NL
      "\t-h, help              : Displays this help message." NL
      "\tc, clean              : Cleans the cache dir and removes the output." NL
      "\tn, new <project-name> : Creates a new subdir with name <project-name>, "
      "creating a default luamake build script." NL
      "\tb, build              : Builds the project based on the `Build` function "
      "defined in the `luamake.lua` file in the current dir." NL
      "\tt, test               : Builds the project based on the `Build` function "
      "in the `luamake.lua` file in the current dir, with the additional macro "
      "`LUAMAKE_TESTS` defined. Then runs the tests defined in the `Test` "
      "function "
      "defined in the `luamake.lua` file in the current dir, displaying the "
      "number of tests that succeeded." NL
      "\tr, run                : Builds the project based on the `Build` function "
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
    return 1;
  case MakeTypes::exit_t::lua_vm_error:
    return 1;
  case MakeTypes::exit_t::config_error:
    return 70;
  case MakeTypes::exit_t::useage_error:
    return 65;
  }
}
