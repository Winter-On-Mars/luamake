#include "luamake_builtins.hpp"

#include "common.hpp"
#include "luamake_error.hpp"

extern "C" {
#include "lua/lua.h"
}

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace luamake_builtins {
using std::array, std::string, std::string_view, std::unique_ptr, std::vector;

static auto parse_compiler_table(lua_State *) -> string;

// TODO: fill this out
// read user luamake.lua to find module dependency
static auto get_include_paths() noexcept -> vector<fs::path> {
  return vector<fs::path>{".", "/usr/include"};
}

// TODO: make this return a bool to check if we hit 0
static auto skip_ws(char const *ch) noexcept -> char const * {
  auto const *local = ch;
  while (*local != 0) {
    switch (*local) {
    case ' ':
      [[fallthrough]];
    case '\t':
      [[fallthrough]];
    case '\n':
      [[fallthrough]];
    case '\r': {
      ++local;
    } break;
    default:
      return local;
    }
  }
  // TODO: error checking when we hit eof
  return local;
}

struct File final {

  // idk expand this later if you want
  enum permissions : unsigned char {
    READ,
  };

  File(fs::path const &path, permissions &&perms) noexcept
      : file(fopen(path.c_str(), perms == READ ? "r" : ".")) {}
  ~File() noexcept {
    if (file != nullptr)
      fclose(file);
  }

  // implicit conversion operator to FILE*
  operator FILE *() const noexcept { return file; }

private:
  FILE *file;
};

// algorithm
// https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function#FNV-1a_hash
static auto constexpr fnv1a(size_t size, char const *buffer) noexcept
    -> size_t {
  auto [hash, fnv1a_prime] = []() -> std::pair<size_t, size_t> {
    if constexpr (sizeof(size_t) == 4) {
      return std::make_pair(0x01000193, 0x811c9dc5);
    } else if constexpr (sizeof(size_t) == 8) {
      return std::make_pair(0xcbf29ce484222325, 0x00000100000001b3);
    } else {
      throw;
    }
  }();

  for (size_t i{0}; i < size; ++i) {
    hash = hash ^ static_cast<size_t>(buffer[i]);
    hash = hash * fnv1a_prime;
  }

  return hash;
}

struct Module final {
  enum Module_t {
    EXE,
    STATIC,
    DYNAMIC,
  };

  static auto make(Module_t type, lua_State *state) noexcept -> Module {
    auto ret_t = Module{};
    ret_t.m.type = type;

    lua_getfield(state, -1, "name");
    ret_t.m.name = lua_tolstring(state, -1, nullptr);

    lua_getfield(state, -2, "root");
    ret_t.m.root = lua_tolstring(state, -1, nullptr);

    lua_getfield(state, -3, "compiler");
    ret_t.m.compiler = parse_compiler_table(state);

    lua_getfield(state, -4, "install_dir");
    ret_t.m.install_dir = lua_tolstring(state, -1, nullptr);

    // TODO: update this to record the number of things we push onto the stack
    // to make sure that this doesn't fuck up the stack
    lua_pop(state,
            4); // might cause an issue? just trying to restore the stack

    return ret_t;
  }

  auto constexpr install_dir() const noexcept -> char const * {
    return m.install_dir;
  }

  auto constexpr name() const noexcept -> char const * { return m.name; }

  auto root() const noexcept -> fs::path { return m.root; }

  auto compiler() const noexcept -> std::string { return m.compiler; }

private:
  struct M {
    Module_t type;
    char const *name;
    fs::path root;
    std::string compiler;
    char const *install_dir;
    // other module deps
  };
  M m;
};

struct EmptyFileName final {
  fs::path path;
  explicit EmptyFileName(fs::path const &path) noexcept : path(path) {}
  auto error() const noexcept -> string {
    return string("In [") + path.string() + "] found empty include path.";
  };
};

struct FileDoesNotExist final {
  fs::path name;
  fs::path parent;

  explicit FileDoesNotExist(fs::path const &fname,
                            fs::path const &parent) noexcept
      : name(fname), parent(parent) {}

  auto error() const noexcept -> string {
    return string("Attempting to open file [") + name.string() +
           string("] That does not exist") +
           (parent == fs::current_path()
                ? string(".")
                : string(" depended on by ") + parent.string() + ".");
  };
};

struct NonTerminatedString final {
  fs::path name;

  explicit NonTerminatedString(fs::path const &fname) noexcept : name(fname) {}

  auto error() const noexcept -> string {
    return string("File [") + name.string() +
           string("] contains a non-terminating string in an include path.");
  };
};

struct MalformedInclude final {
  fs::path name;

  explicit MalformedInclude(fs::path const &fname) noexcept : name(fname) {}

  auto error() const noexcept -> string {
    return string("File [") + name.string() +
           "] contains a malformed include path";
  };
};

struct CFileAPIError final {
  string message;
  auto error() const noexcept -> string { return message; }
};

using SourceFileErr =
    std::variant<EmptyFileName, FileDoesNotExist, NonTerminatedString,
                 MalformedInclude, CFileAPIError>;

struct SourceFile final {
  enum SourceFile_t {
    IMPL,
    HEADER,
    SYSTEM,
    MISC,
  };

  static auto make(fs::path const &root,
                   fs::path const &parent = fs::current_path()) noexcept
      -> Result<SourceFile, SourceFileErr> {
    using Ok = Result<SourceFile, SourceFileErr>::Ok;
    using Err = Result<SourceFile, SourceFileErr>::Err;
    std::cerr << "calling make with path = [" << root << "]\n";

    auto res = SourceFile();
    auto const ext = root.extension();

    res.m.type = determine_file_type(ext);

    res.m.path = root;

    if (auto opt_err = res.analyze_deps_and_hash(parent); opt_err.has_value()) {
      return Err(std::move(opt_err.value()));
    }

    return Ok(std::move(res));
  }

  SourceFile(SourceFile const &) = delete;
  SourceFile &operator=(SourceFile const &) = delete;

  SourceFile(SourceFile &&) = default;
  SourceFile &operator=(SourceFile &&) = default;

  // displays the function in a pseudo json format
  auto display(int const depth = 0) const noexcept -> void {
    using std::cout;
    auto const indents = [](int const depth) -> string {
      auto res = string(static_cast<size_t>(depth), '\t');
      return res;
    }(depth);
    cout << indents << "{\n";
    cout << indents << "type = \"";
    switch (m.type) {
    case IMPL:
      cout << "IMPL";
      break;
    case HEADER:
      cout << "HEADER";
      break;
    case SYSTEM:
      cout << "SYSTEM";
      break;
    case MISC:
      cout << "MISC";
      break;
    };
    cout << "\",\n";

    // path already include the ""
    cout << indents << "path = " << m.path << ",\n";
    cout << indents << std::hex << "hash = " << m.hash << ",\n";

    cout << indents << "deps = [\n";
    for (auto const &sf_ptr : m.deps)
      sf_ptr->display(depth + 1);
    cout << indents << "],\n";

    cout << indents << "}\n";
  }

private:
  struct M final {
    SourceFile_t type;
    fs::path path;
    vector<unique_ptr<SourceFile>> deps;
    size_t hash;
    // TODO:
    // std::thread hashing_thread;
    // std::thread dep_analyzer_thread;
  } m;

  static auto determine_file_type(fs::path const &ext) noexcept
      -> SourceFile_t {
    if (ext == ".cpp" || ext == ".cxx" || ext == ".cc" || ext == ".c") {
      return IMPL;
    }
    if (ext == ".hpp" || ext == ".hxx" || ext == ".hh" || ext == ".h") {
      return HEADER;
    }
    return MISC;
  }

  auto analyze_deps_and_hash(fs::path const &parent) noexcept
      -> std::optional<SourceFileErr> {
    using Result = Result<SourceFile, SourceFileErr>;

    auto file = File(m.path.c_str(), File::READ);
    if (!file) {
      return FileDoesNotExist(m.path, parent);
    }
    fseek(file, 0, SEEK_END);
    auto const _fsize = ftell(file);
    if (_fsize == -1) {
      return CFileAPIError(string(strerror(errno)));
    }
    auto const fsize = static_cast<size_t>(_fsize);
    rewind(file);

    // TODO: remove the null terminator b/c it's only needed for debugging
    auto const fcontent = std::make_unique<char[]>(fsize + 1);
    // TODO: check that we actually read the whole file
    auto const amount_read = fread(fcontent.get(), sizeof(char), fsize, file);
    if (amount_read != fsize) {
      return CFileAPIError(string(strerror(errno)));
    }
    fcontent[fsize] = 0;

    m.hash = fnv1a(fsize, fcontent.get());

    auto constexpr include_prefix = string_view{"#include"};
    auto const potential_include_dirs = get_include_paths();

    auto in_string = false;

    for (auto const *ch = fcontent.get(); *ch != 0; ++ch) {
      auto const is_hash = *ch == '#';
      in_string = *ch == '"';
      if (is_hash && !in_string &&
          strncmp(ch, include_prefix.data(), include_prefix.size()) == 0) {
        ch += include_prefix.size();
        ch = skip_ws(ch);

        switch (*ch) {
        case '"': {
          // TODO: local include
          ++ch;
          auto const *end_of_include_string = ch;
          while (*end_of_include_string != 0 && *end_of_include_string != '"') {
            ++end_of_include_string;
          }

          if (*end_of_include_string == 0) {
            return NonTerminatedString(m.path);
          }

          auto const include_string_size = end_of_include_string - ch;
          switch (include_string_size) {
          case 0: {
            return EmptyFileName(m.path);
          } break;
          default: {
            auto const file_name =
                fs::path(string_view{ch, end_of_include_string});
            if (file_name.stem() == m.path.stem()) {
              auto const file_name_ext =
                  determine_file_type(file_name.extension());
              if (file_name_ext == HEADER) {
              }
            }
            auto const path = m.path.parent_path() / file_name;

            std::cout << "file_name = [" << file_name << "]\n";
            std::cout.flush();
            std::cout << "path = [" << path << "]\n";
            std::cout.flush();

            auto sf = SourceFile::make(path);
            switch (sf) {
            case Result::OK: {
              m.deps.emplace_back(std::make_unique<SourceFile>(sf.get()));
            } break;
            case Result::ERR: {
              return sf.err();
            } break;
            }
          } break;
          }
        } break;
        case '<': {
          std::cout << "found global [" << ch << "]\n";
          std::cout.flush();
          // TODO: global/module include
        } break;
        default: {
          std::cout << "unknown char [" << *ch << "]\n";
          std::cout.flush();
          // TODO: report error malformed #include directive
        } break;
        }
      }
    }
    return std::nullopt;
  }

  SourceFile() = default;
  friend Result<SourceFile, SourceFileErr>;
};

auto clang(lua_State *state) -> int {
  fn_print();

  auto num_args = lua_gettop(state);
  if (num_args != 1) {
    lua_pushstring(state, "Too many arguments");
    return lua_error(state);
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

  exit_fn_print();

  return 1;
}

static auto parse_compiler_table(lua_State *state) -> string {
  auto str = string();

  lua_getfield(state, -1, "compiler");
  str += lua_tolstring(state, -1, nullptr);

  lua_getfield(state, -2, "optimize");
  str += " -";
  str += lua_tolstring(state, -1, nullptr);

  lua_getfield(state, -3, "warnings");
  auto tbl_idx = -1;
  auto num_warnings = lua_rawlen(state, tbl_idx);
  for (auto i = lua_Unsigned{1}; i <= num_warnings; ++i) {
    switch (lua_geti(state, tbl_idx, static_cast<lua_Integer>(i))) {
    case LUA_TSTRING:
      str += " -";
      str += lua_tolstring(state, -1, nullptr);
      break;
    default: // TODO: propogate error up
      lua_pushstring(state, "Incorrect type in `warnings` table");
      lua_error(state);
      return str;
    }
    --tbl_idx;
  }
  lua_pop(state, 3 + static_cast<int>(num_warnings));
  return str;
}

static auto install_exe(lua_State *state) -> int {
  using Result = Result<SourceFile, SourceFileErr>;
  fn_print();

  auto num_args = lua_gettop(state);
  if (num_args != 1) {
    lua_pushstring(state, "Too many arguments.");
    return lua_error(state);
  }

  auto main_mod = Module::make(Module::EXE, state);

  auto ec = std::error_code{};
  if (fs::create_directories(
          fs::path(
              std::format("{}/{}.o", main_mod.install_dir(), main_mod.name())),
          ec);
      ec) {
    std::cerr << ec.message() << '\n';
    lua_pushstring(state, "Unable to create directory");
    return lua_error(state);
  }
  ec.clear();

  // TODO: build dep tree
  auto maybe_exe_root = SourceFile::make(main_mod.root());
  switch (maybe_exe_root) {
  case Result::OK: {
    auto const &exe_root = maybe_exe_root.get();

    exe_root.display();

    // compile the objects
    auto invoked_command =
        std::format("{} -c {} -o {}/{}.o/{}.o", main_mod.compiler(),
                    main_mod.root().c_str(), main_mod.install_dir(),
                    main_mod.name(), main_mod.root().stem().c_str());
    std::cerr << "Invoking [" << invoked_command << "]\n";

    /*
    if (system(invoked_command.c_str()) != 0) {
      // this should be fine bc lua will intern the string(?)
      lua_pushfstring(state, "Error invoking [%s]\n", invoked_command.c_str());
      return lua_error(state);
    }
    */

    // compile the program
    invoked_command = std::format(
        "{} {}/{}.o/{}.o -o {}/{}", main_mod.compiler(), main_mod.install_dir(),
        main_mod.name(), main_mod.root().stem().c_str(), main_mod.install_dir(),
        main_mod.name());
    std::cerr << "Invoking [" << invoked_command << "]\n";

    /*
    if (system(invoked_command.c_str()) != 0) {
      lua_pushfstring(state, "Error invoking [%s]\n", invoked_command.c_str());
      return lua_error(state);
    }
    */

    exit_fn_print();
    return 0;
  } break;
  case Result::ERR: {
    auto const msg = std::visit([](auto &&e) { return e.error() + '\n'; },
                                maybe_exe_root.err());

    lua_pushstring(state, msg.c_str());
    return lua_error(state);
  } break;
  }
}

static auto install_static(lua_State *L) -> int {
  fn_print();

  auto num_args = lua_gettop(L);
  if (num_args != 1) {
    lua_pushstring(L, "Too many arguments");
    return lua_error(L);
  }

  lua_getfield(L, -1, "name");
  auto const name = string(lua_tolstring(L, -1, nullptr));

  lua_getfield(L, -2, "invoke_command");
  auto const install_command = string(lua_tolstring(L, -1, nullptr));

  expr_dbg(install_command);

  exit_fn_print();
  return 0;
}

auto make_builder_obj(lua_State *state, std::string_view const builder_obj)
    -> void {
  lua_createtable(state, 0, 2);

  lua_pushstring(state, ".");
  lua_setfield(state, -2, "install_dir");

  lua_pushcfunction(state, install_exe);
  lua_setfield(state, -2, "install_exe");

  lua_pushcfunction(state, install_static);
  lua_setfield(state, -2, "install_static");

  // TODO: add the functions install_dynamic
}
} // namespace luamake_builtins
