#include "luamake_builtins.hpp"

#include "common.hpp"
#include "luamake_error.hpp"

extern "C" {
#include "lua/lua.h"
}

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <endian.h>
#include <filesystem>
#include <format>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace luamake_builtins {
using std::pair, std::array, std::string, std::string_view, std::unique_ptr,
    std::vector;

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
  enum permissions : unsigned char {
    READ = 1 << 0,
    WRITE = 1 << 1,
    BINARY = 1 << 2,
  };

  constexpr File(fs::path const &path, permissions &&perms) noexcept
      : file(nullptr) {
    char max_length_perms[] = {0, 0, 0,
                               0}; // this should be 5 or 7 from man fread
    if ((perms & READ) == READ)
      max_length_perms[0] = 'r';
    if ((perms & WRITE) == WRITE)
      max_length_perms[max_length_perms[0] != 0 ? 1 : 0] = 'w';
    if ((perms & BINARY) == BINARY)
      max_length_perms[max_length_perms[0] != 0
                           ? max_length_perms[1] != 0 ? 2 : 1
                           : 0] = 'b';

    file = fopen(path.c_str(), max_length_perms);
  }
  constexpr ~File() noexcept {
    if (file != nullptr)
      fclose(file);
  }

  // implicit conversion operator to FILE*
  operator FILE *() const noexcept { return file; }

  auto write(void const *__restrict ptr, size_t size, size_t amount) noexcept
      -> size_t {
    return fwrite(ptr, size, amount, file);
  }

  auto flush() noexcept -> void { fflush(file); }

private:
  FILE *file;
};

auto constexpr operator|(File::permissions lhs, File::permissions rhs) noexcept
    -> File::permissions {
  return static_cast<File::permissions>(
      static_cast<std::underlying_type_t<File::permissions>>(lhs) |
      static_cast<std::underlying_type_t<File::permissions>>(rhs));
}

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
  explicit CFileAPIError(string const &message) noexcept : message(message) {}
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

    auto file = File(root, File::READ);
    if (!file)
      return Err(FileDoesNotExist(root, parent));

    auto maybe_fsize = SourceFile::read_and_report_fsize(file);
    switch (maybe_fsize) {
    case decltype(maybe_fsize)::OK: {
      auto &&[fcontent, fsize] = maybe_fsize.get();
      auto res = SourceFile();
      res.m.path = root;

      auto hash_fut = std::async(
          std::launch::async,
          [](size_t size, char const *fcontent) {
            return fnv1a(size, fcontent);
          },
          fsize, fcontent);

      auto const ext = res.m.path.extension();
      res.m.type = SourceFile::determine_file_type(ext);

      if (res.m.type == HEADER) {
        // try to open impl file
        auto const potential_impl =
            (res.m.path.parent_path() / res.m.path.stem()).string();
        std::cout << "checking to see if [" << potential_impl << "] exists\n";
        std::cout.flush();
        if (fs::exists(fs::path(potential_impl + ".cpp"))) {
          std::cout << "found [" << fs::path(potential_impl + ".cpp") << "]\n";
          std::cout.flush();
          auto maybe_impl =
              SourceFile::make(fs::path(potential_impl + ".cpp"), res.m.path);
          switch (maybe_impl) {
          case decltype(maybe_impl)::OK: {
            res.m.deps.emplace_back(
                std::make_unique<SourceFile>(maybe_impl.get()));
          } break;
          case decltype(maybe_impl)::ERR:
            free((void *)fcontent);
            return Err(maybe_impl.err());
          }
        } else if (fs::exists(fs::path(potential_impl + ".c"))) {
          std::cout << "found [" << fs::path(potential_impl + ".c") << "]\n";
          std::cout.flush();
          auto maybe_impl =
              SourceFile::make(fs::path(potential_impl + ".c"), res.m.path);
          switch (maybe_impl) {
          case decltype(maybe_impl)::OK: {
            res.m.deps.emplace_back(
                std::make_unique<SourceFile>(maybe_impl.get()));
          } break;
          case decltype(maybe_impl)::ERR:
            free((void *)fcontent);
            return Err(maybe_impl.err());
          }
        } else {
          // idk probably a header only library
        }
      }

      auto opt_deps =
          SourceFile::analyze_dep(res.m.path, parent, file, fcontent);
      switch (opt_deps) {
      case decltype(opt_deps)::OK: {
        // have to move the new deps over, otherwise we'll be clobbering the
        // impl deps
        auto tmp_dep = std::move(opt_deps.get());
        res.m.deps.reserve(res.m.deps.size() + tmp_dep.size());
        std::move(tmp_dep.begin(), tmp_dep.end(),
                  std::back_inserter(res.m.deps));

        // make sure the hashing is finished
        res.m.hash = hash_fut.get();

        // clean up and return
        free((void *)fcontent);
        return Ok(std::move(res));
      } break;
      case decltype(opt_deps)::ERR:
        free((void *)fcontent);
        return Err(opt_deps.err());
      }
    } break;
    case decltype(maybe_fsize)::ERR:
      return Err(maybe_fsize.err());
    }
  }

  SourceFile(SourceFile const &) = delete;
  SourceFile &operator=(SourceFile const &) = delete;

  SourceFile(SourceFile &&) = default;
  SourceFile &operator=(SourceFile &&) = default;

  // displays the function in a pseudo json format
  auto display(std::ostream &out, int const depth = 0) const noexcept -> void {
    auto const indents = [](int const depth) -> string {
      auto res = string(static_cast<size_t>(depth), '\t');
      return res;
    }(depth);
    out << indents << "{\n";
    out << indents << "\"type\":\"";
    switch (m.type) {
    case IMPL:
      out << "IMPL";
      break;
    case HEADER:
      out << "HEADER";
      break;
    case SYSTEM:
      out << "SYSTEM";
      break;
    case MISC:
      out << "MISC";
      break;
    };
    out << "\",\n";

    // path already include the ""
    out << indents << "\"path\":" << m.path << ",\n";
    out << std::hex << indents << "\"hash\":" << m.hash << ",\n";

    out << indents << "\"deps\":[\n";
    for (int i = 0; auto const &sf_ptr : m.deps) {
      sf_ptr->display(out, depth + 1);
      if (i != m.deps.size() - 1)
        out << ",\n";
      ++i;
    }
    out << indents << "]\n";

    out << indents << "}\n";
  }

  // TODO: add better error handling
  auto serialize(fs::path const &path) const noexcept -> void {
    auto outfile = File(path, File::WRITE | File::BINARY);
    if (outfile == nullptr) {
      return;
    }

    auto amount_written = outfile.write(&m.type, sizeof(decltype(M::type)), 1);

    auto bytes = htobe64(m.hash);
    amount_written += outfile.write(&bytes, sizeof(decltype(M::hash)), 1);

    // i hope this doesn't alloc that'd be annoying
    auto const path_len = m.path.string().size();
    amount_written += outfile.write(&path_len, sizeof(decltype(path_len)), 1);
    amount_written += outfile.write(m.path.c_str(), sizeof(char), path_len);

    // TODO: write out the deps
    // outfile.write(&(m.deps.size()), sizeof(decltype(M::deps.size())), 1);
    expr_dbg(amount_written);
    outfile.flush();
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

  static auto analyze_dep(fs::path const &path, fs::path const &parent,
                          FILE *file, char const *fcontent) noexcept
      -> Result<decltype(SourceFile::M::deps), SourceFileErr> {
    using Ok = Result<decltype(SourceFile::M::deps), SourceFileErr>::Ok;
    using Err = Result<decltype(SourceFile::M::deps), SourceFileErr>::Err;
    std::cout.flush();

    auto res = vector<unique_ptr<SourceFile>>();

    auto constexpr include_prefix = string_view{"#include"};
    auto const potential_include_dirs = get_include_paths();

    auto in_string = false;

    for (auto const *ch = fcontent; *ch != 0; ++ch) {
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
            return Err(NonTerminatedString(path));
          }

          auto const include_string_size = end_of_include_string - ch;
          switch (include_string_size) {
          case 0: {
            return Err(EmptyFileName(path));
          } break;
          default: {
            auto const include_file =
                fs::path(string_view{ch, end_of_include_string});
            if (include_file.stem() == path.stem()) {
              auto const include_f_ext =
                  determine_file_type(include_file.extension());
              auto const path_ext = determine_file_type(path.extension());
              if (include_f_ext == HEADER && path_ext == IMPL) {
                continue;
                // ignore this path
              }
            }
            auto const dep_path = path.parent_path() / include_file;

            auto sf = SourceFile::make(dep_path, path);
            switch (sf) {
            case decltype(sf)::OK: {
              res.emplace_back(std::make_unique<SourceFile>(sf.get()));
            } break;
            case decltype(sf)::ERR: {
              return Err(sf.err());
            } break;
            }
          } break;
          }
        } break;
        case '<': {
          // std::cout << "found global [" << ch << "]\n";
          // std::cout.flush();
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
    return Ok(std::move(res));
  }

  static auto read_and_report_fsize(FILE *file) noexcept
      -> Result<pair<char const *, size_t>, SourceFileErr> {
    using Ok = Result<pair<char const *, size_t>, SourceFileErr>::Ok;
    using Err = Result<pair<char const *, size_t>, SourceFileErr>::Err;
    if (fseek(file, 0, SEEK_END) == -1)
      return Err(CFileAPIError(strerror(errno)));

    auto const _fsize = ftell(file);
    if (_fsize == -1)
      return Err(CFileAPIError(strerror(errno)));

    auto fsize = static_cast<size_t>(_fsize);
    rewind(file);

    // std::cout << "calling malloc with size = [" << sizeof(char) * fsize
    //             << "]\n";
    // std::cout.flush();
    auto *fcontent = (char *)malloc(sizeof(char) * fsize + 1);
    if (fcontent == nullptr)
      return Err(CFileAPIError(strerror(errno)));

    if (auto const amount_read = fread(fcontent, sizeof(char), fsize, file);
        amount_read != fsize)
      return Err(CFileAPIError(strerror(errno)));
    fcontent[fsize] = 0;
    return Ok(std::make_pair(fcontent, fsize));
  }

  SourceFile() = default;
  SourceFile(SourceFile::M &&m) noexcept : m(std::move(m)) {}
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

    // auto cache_file = std::ofstream("./.cache.json");
    exe_root.display(std::cout);
    exe_root.serialize(".test.bin");

    // compile the objects
    /*
    auto invoked_command =
        std::format("{} -c {} -o {}/{}.o/{}.o", main_mod.compiler(),
                    main_mod.root().c_str(), main_mod.install_dir(),
                    main_mod.name(), main_mod.root().stem().c_str());
    std::cerr << "Invoking [" << invoked_command << "]\n";
    */

    /*
    if (system(invoked_command.c_str()) != 0) {
      // this should be fine bc lua will intern the string(?)
      lua_pushfstring(state, "Error invoking [%s]\n", invoked_command.c_str());
      return lua_error(state);
    }
    */

    // compile the program
    /*
    invoked_command = std::format(
        "{} {}/{}.o/{}.o -o {}/{}", main_mod.compiler(), main_mod.install_dir(),
        main_mod.name(), main_mod.root().stem().c_str(), main_mod.install_dir(),
        main_mod.name());
    std::cerr << "Invoking [" << invoked_command << "]\n";
    */

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
