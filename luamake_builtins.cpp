#include "luamake_builtins.hpp"

#include "common.hpp"
#include "luamake_error.hpp"

extern "C" {
#include "lua/lua.h"
}

#include <array>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <endian.h>
#include <exception>
#include <filesystem>
#include <format>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#define LUA_ASSERT(L, A, B, ERROR)                                             \
  if ((A) != (B)) {                                                            \
    lua_pushstring((L), (ERROR));                                              \
    return lua_error((L));                                                     \
  }

#define LUA_ASSERT_FORMAT(L, name, A, B, fmt, ...)                             \
  if (auto const name = A; (name) != (B)) {                                    \
    lua_pushfstring((L), fmt, __VA_ARGS__);                                    \
    return lua_error((L));                                                     \
  }

namespace luamake_builtins {
using std::pair, std::array, std::string, std::string_view, std::vector;

namespace {
// TODO: fill this out
// read user luamake.lua to find module dependency
auto get_include_paths() noexcept -> vector<fs::path> {
  return vector<fs::path>{".", "/usr/include"};
}

// TODO: make this return a bool to check if we hit 0
auto skip_ws(char const *ch) noexcept -> char const * {
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

// algorithm
// https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function#FNV-1a_hash
auto constexpr fnv1a(size_t size, char const *buffer) noexcept -> size_t {
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

#if false
// helper function for displaying every byte of the string_view
auto display_string_view(string_view const str) noexcept -> void {
  for (auto i = size_t{}; i != str.length(); ++i) {
    std::cout << str[i] << '-';
  }
  std::cout.flush();
}
#endif

struct Compiler;
struct CompilationPool;

struct MissingField final {
  string_view field_name;
  MissingField(string_view &&field_name) noexcept : field_name(field_name) {}
  auto error() const noexcept -> string {
    return string("Required field [") + field_name.data() +
           string("] could not be found when constructing a module.");
  }
};

using ModuleErr = std::variant<MissingField>;

struct Module final {
  enum Module_t {
    EXE,
    STATIC,
    DYNAMIC,
  };

  static auto make(Module_t &&type, lua_State *state) noexcept
      -> Result<Module, ModuleErr>;

  auto constexpr install_dir() const noexcept -> char const * {
    return m.install_dir;
  }

  auto constexpr name() const noexcept -> char const * { return m.name; }

  auto root() const noexcept -> fs::path { return m.root; }

  auto compiler() const noexcept -> std::string { return m.compiler; }

private:
  static auto parse_compiler_table(lua_State *state) -> string;

  struct M {
    Module_t type;
    char const *name;
    fs::path root; // TOOD: have this field be a union of a vector<fs::path> and
                   // fs::path
    std::string compiler;
    char const *install_dir;
    // other module deps
  };
  M m;
};

// TODO: update error handling
auto Module::make(Module_t &&type, lua_State *state) noexcept
    -> Result<Module, ModuleErr> {
  using Ok = Result<Module, ModuleErr>::Ok;
  using Err = Result<Module, ModuleErr>::Err;
  auto ret_t = Module{};
  ret_t.m.type = type;

  lua_getfield(state, -1, "name");
  if (lua_type(state, -1) == LUA_TNIL)
    return Err(MissingField("name"));
  ret_t.m.name = lua_tolstring(state, -1, nullptr);

  // TODO: switch on type, and parse this field different
  lua_getfield(state, -2, "root");
  if (lua_type(state, -1) == LUA_TNIL)
    return Err(MissingField("root"));
  ret_t.m.root = lua_tolstring(state, -1, nullptr);

  lua_getfield(state, -3, "compiler");
  if (lua_type(state, -1) == LUA_TNIL)
    return Err(MissingField("compiler"));
  ret_t.m.compiler = Module::parse_compiler_table(state);

  lua_getfield(state, -4, "install_dir");
  if (lua_type(state, -1) == LUA_TNIL)
    return Err(MissingField("install_dir"));
  ret_t.m.install_dir = lua_tolstring(state, -1, nullptr);

  lua_pop(state, 3);

  return Ok(std::move(ret_t));
}

auto Module::parse_compiler_table(lua_State *state) -> string {
  auto str = string();

  auto const compiler_idx = lua_absindex(state, -1);

  lua_getfield(state, compiler_idx, "compiler");
  str += lua_tolstring(state, -1, nullptr);

  lua_getfield(state, compiler_idx, "optimize");
  str += " -";
  str += lua_tolstring(state, -1, nullptr);

  lua_getfield(state, compiler_idx, "warnings");
  auto const warnings_idx = lua_absindex(state, -1);

  lua_pushnil(state);
  while (lua_next(state, warnings_idx) != 0) {
    if (lua_type(state, -1) != LUA_TSTRING) {
      lua_pushstring(
          state,
          "Incorrect type in `warnings` table"); // TODO: update this to include
                                                 // the found type
      lua_error(state);
      return "";
    }
    str += " -";
    str += lua_tolstring(state, -1, nullptr);
    lua_pop(state, 1);
  }

  lua_getfield(state, compiler_idx, "opt_args");
  auto const opt_arg_idx = lua_absindex(state, -1);

  lua_pushnil(state);
  while (lua_next(state, opt_arg_idx) != 0) {
    if (lua_type(state, -1) != LUA_TSTRING) {
      lua_pushstring(state, "Incorrect type in `opt_args` table");
      lua_error(state);
      return "";
    }
    str += ' ';
    str += lua_tolstring(state, -1, nullptr);

    lua_pop(state, 1);
  }

  lua_pop(state, 4);
  return str;
}

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

struct CFileAPI final {
  string message;
  explicit CFileAPI(string const &message) noexcept : message(message) {}
  auto error() const noexcept -> string { return message; }
};

struct MemoryAlloc final {
  char const *message;
  explicit MemoryAlloc(char const *const message) noexcept : message(message) {}
  auto error() const noexcept -> string {
    return std::format("Error while allocating memory in function [{}]",
                       message);
  }
};

// TODO: rename this to dep_tree
using SourceFileErr =
    std::variant<EmptyFileName, FileDoesNotExist, NonTerminatedString,
                 MalformedInclude, CFileAPI, MemoryAlloc>;

struct SourceFile final {
  enum SourceFile_t : unsigned char {
    IMPL,
    HEADER,
    SYSTEM,
    MISC,
  };

  static auto make(fs::path const &root,
                   fs::path const &parent = fs::current_path()) noexcept
      -> Result<SourceFile, SourceFileErr>;

  SourceFile(SourceFile const &) = delete;
  SourceFile &operator=(SourceFile const &) = delete;

  SourceFile(SourceFile &&) = default;
  SourceFile &operator=(SourceFile &&) = default;

  ~SourceFile() noexcept = default;

  // displays the function in a pseudo json format
  auto display(std::ostream &out, unsigned int const depth = 0) const noexcept
      -> void;

  /*
  // TODO: add better error handling
  auto serialize(fs::path const &path) const noexcept -> void;

  // TODO: we're just assuming that the path is well constructed
  // so add some error handling to this function
  [[nodiscard(
      "We spent all this time deserializing you better use the result")]]
  static auto deserialize(fs::path const &path) noexcept -> SourceFile;
  */

  [[nodiscard]]
  static auto determine_file_type(fs::path &&ext) noexcept -> SourceFile_t {
    if (ext == ".cpp" || ext == ".cxx" || ext == ".cc" || ext == ".c") {
      return IMPL;
    }
    if (ext == ".hpp" || ext == ".hxx" || ext == ".hh" || ext == ".h") {
      return HEADER;
    }
    return MISC;
  }

private:
  struct OwnedString final {
    char *buffer;
    size_t size;
    size_t capacity;

    explicit OwnedString(char *buffer, size_t size) noexcept;
    constexpr OwnedString() noexcept;

    constexpr OwnedString(OwnedString &&that) noexcept;

    constexpr auto operator=(OwnedString &&that) noexcept -> OwnedString &;

    ~OwnedString() noexcept;

    OwnedString(OwnedString const &) = delete;
    OwnedString &operator=(OwnedString const &) = delete;

    auto append(string &&) noexcept -> void;
  };

  // a parallel array for all of the source files
  // NOTE: this could be pushed further, and we could have a
  // memory allocator as a part of this struct, then just
  // clearing the memory allocator would act as the destructor
  // TODO: if performance becomes an issue, it might be good to switch this to a
  // hash set for the `find` function
  struct M final {
    struct StringViews final {
      unsigned int start;
      unsigned int end;
    };

    size_t num_files;
    size_t cap_files;
    OwnedString all_paths;
    std::unique_ptr<SourceFile_t[]> types;
    std::unique_ptr<StringViews[]> files;
    std::unique_ptr<vector<unsigned int>[]> deps;
    std::unique_ptr<size_t[]> hashes;

    [[nodiscard]]
    static auto make(size_t const num_files = 8) noexcept
        -> Result<M, SourceFileErr>;
    [[nodiscard]]
    auto append_path(fs::path const &) noexcept
        -> pair<unsigned int, unsigned int>;
    [[nodiscard]]
    auto append_dep(fs::path const &root, size_t const parent_idx) noexcept
        -> Opt<SourceFileErr>;
    [[nodiscard]]
    auto get_path(size_t const) const noexcept -> fs::path;
    [[nodiscard]]
    auto find(string_view const string) const noexcept
        -> std::tuple<bool, unsigned int, unsigned int>;
  } m;

  // this should probably returned a FixedString, we don't need the size and
  // capacity
  static auto get_file_content(FILE *file) noexcept
      -> Result<OwnedString, SourceFileErr>;

  auto display_impl(std::ostream &out, unsigned int const depth,
                    unsigned int const idx) const noexcept -> void;
  /*
  auto serialize_impl(File &file) const noexcept -> void;

  static auto deserialize_impl(File &file) noexcept -> SourceFile;
  */

  SourceFile() = default;
  SourceFile(SourceFile::M &&m) noexcept : m(std::move(m)) {}
  friend Result<SourceFile, SourceFileErr>;
  friend Compiler;
  friend CompilationPool;
};

SourceFile::OwnedString::OwnedString(char *buffer, size_t size) noexcept
    : buffer(buffer), size(0), capacity(size) {}
constexpr SourceFile::OwnedString::OwnedString() noexcept
    : buffer(nullptr), size(0), capacity(0) {}

constexpr SourceFile::OwnedString::OwnedString(
    SourceFile::OwnedString &&that) noexcept
    : buffer(that.buffer), size(that.size), capacity(that.capacity) {
  that.buffer = nullptr;
  that.size = 0;
  that.capacity = 0;
}

constexpr auto
SourceFile::OwnedString::operator=(SourceFile::OwnedString &&that) noexcept
    -> SourceFile::OwnedString & {
  buffer = that.buffer;
  size = that.size;
  capacity = that.capacity;
  return *this;
}

SourceFile::OwnedString::~OwnedString() noexcept {
  if (buffer != nullptr)
    free((void *)buffer);
}

auto SourceFile::OwnedString::append(string &&str) noexcept -> void {
  auto const str_len = str.length();
  if (!(size < capacity - str_len - 1)) {
    /* resize */
    auto next_cap = 3 * (capacity + str_len + 1) / 2;
    buffer = (char *)realloc(buffer, next_cap * sizeof(char));
    if (buffer == nullptr) {
      std::cerr << "Unable to realloc [" << next_cap << "] bytes needed\n";
      std::terminate();
    }
    capacity = next_cap;
  }
  memcpy(buffer + size, str.data(), str_len);
  size += str_len;
  buffer[size] = 0;
  ++size;
}

auto SourceFile::M::make(size_t const num_files) noexcept
    -> Result<SourceFile::M, SourceFileErr> {
  using Ok = Result<SourceFile::M, SourceFileErr>::Ok;
  using Err = Result<SourceFile::M, SourceFileErr>::Err;

  try {
    auto const paths_size = num_files * (sizeof(char) * 15 + 1);
    auto *paths = (char *)malloc(paths_size);
    if (paths == nullptr)
      return Err(CFileAPI(strerror(errno)));
    memset(paths, 0, paths_size);

    auto types = std::make_unique<SourceFile_t[]>(num_files);
    auto files = std::make_unique<StringViews[]>(num_files);
    auto deps = std::make_unique<vector<unsigned int>[]>(num_files);
    auto hashes = std::make_unique<size_t[]>(num_files);

    return Ok(M{0, num_files, OwnedString(paths, paths_size), std::move(types),
                std::move(files), std::move(deps), std::move(hashes)});
  } catch (std::exception const &e) {
    return Err(MemoryAlloc(e.what()));
  }
}

auto SourceFile::M::append_path(fs::path const &path) noexcept
    -> pair<unsigned int, unsigned int> {
  if (auto &&[found, start, end] = find(path.c_str()); found) {
    return std::make_pair(start, end);
  }
  auto start = all_paths.size;
  all_paths.append(path.string());
  auto end = all_paths.size;

  if (start >= std::numeric_limits<unsigned int>::max() ||
      end >= std::numeric_limits<unsigned int>::max()) {
    std::cerr << std::format(
        "Damn you have a lot of path strings, more than [{}], idk see about "
        "opening an issue to change how the indexing work, increasing the size "
        "of the StringViews class?",
        std::numeric_limits<unsigned int>::max());
    std::terminate();
  }

  return std::make_pair(static_cast<unsigned int>(start),
                        static_cast<unsigned int>(end));
}

auto SourceFile::M::append_dep(fs::path const &root,
                               size_t const parent_idx) noexcept
    -> Opt<SourceFileErr> {
  using None = Opt<SourceFileErr>::None;
  using Err = Opt<SourceFileErr>::Err;

  auto file = File(root, File::READ);
  if (!file)
    return Err(FileDoesNotExist(root, get_path(parent_idx)));

  auto maybe_file_content = SourceFile::get_file_content(file);
  if (maybe_file_content == decltype(maybe_file_content)::ERR)
    return Err(maybe_file_content.err());

  auto &&[fcontent, _, fsize] = maybe_file_content.get();

  auto const root_idx = num_files;
  assert(root_idx < std::numeric_limits<unsigned int>::max());
  deps[parent_idx].push_back(static_cast<unsigned int>(root_idx));
  ++num_files;

  auto &&[start, end] = append_path(root);
  auto hash_fut = std::async(std::launch::async, [fcontent, fsize]() {
    return fnv1a(fsize, fcontent);
  });
  auto const ftype = SourceFile::determine_file_type(root.extension());
  if (ftype == SourceFile::HEADER) {
    auto constexpr potential_extensions = array<string_view, 2>{{".cpp", ".c"}};
    auto const potential_impl = (root.parent_path() / root.stem()).string();
    for (auto const &potential_extension : potential_extensions) {
      auto const possible_path =
          fs::path(potential_impl + potential_extension.data());
      if (fs::exists(possible_path)) {
        if (auto m_error = append_dep(possible_path, root_idx); !m_error.ok()) {
          return Err(m_error.get());
        }
      }
    }
    // HOL
  }

  types[root_idx] = ftype;
  files[root_idx].start = start;
  files[root_idx].end = end;

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
        ++ch;
        auto const *end_of_include_string = ch;
        while (*end_of_include_string != 0 && *end_of_include_string != '"') {
          ++end_of_include_string;
        }

        if (*end_of_include_string == 0)
          return Err(NonTerminatedString(root));

        auto const include_string_size = end_of_include_string - ch;
        if (include_string_size == 0)
          return Err(EmptyFileName(root));

        auto const include_file =
            fs::path(string_view{ch, end_of_include_string});

        if (include_file.stem() == root.stem()) {
          auto const include_f_ext =
              SourceFile::determine_file_type(include_file.extension());
          auto const path_ext =
              SourceFile::determine_file_type(root.extension());
          if (include_f_ext == HEADER && path_ext == IMPL) {
            continue; // ignore this path
          }
        }

        auto const dep_path = root.parent_path() / include_file;

        if (auto m_error = append_dep(dep_path, root_idx); !m_error.ok())
          return Err(m_error.get());
      } break;
      case '<': {
        // TODO: global include
      } break;
      default: {
        std::cerr << "unknown char [" << *ch << "]\n";
      } break;
      }
    }
  }

  hashes[root_idx] = hash_fut.get();

  return None{};
}

auto SourceFile::M::get_path(size_t const idx) const noexcept -> fs::path {
  auto &&[start, end] = files[idx];
  return fs::path(all_paths.buffer + start, all_paths.buffer + end);
}

// this could (and probably should (if possible)) be rewritten to use the files
// array(?)
auto SourceFile::M::find(string_view const path) const noexcept
    -> std::tuple<bool, unsigned int, unsigned int> {
  auto const *start = all_paths.buffer;
  auto const *current = all_paths.buffer;
  auto end = size_t{};

  while (end != all_paths.size) {
    if (all_paths.buffer[end] == 0) {
      current = all_paths.buffer + end;
      // check
      auto const path_view = string_view{start, current};
      if (path_view.size() == path.size() &&
          *path_view.data() == *path.data() &&
          strncmp(path_view.data(), path.data(), path_view.size()) == 0) {
        return std::make_tuple(
            true, static_cast<unsigned int>(start - all_paths.buffer),
            static_cast<unsigned int>(end));
      }
      start = current + 1;
    }
    ++end;
  }

  return std::make_tuple(false, 0, 0);
}

// TODO: extract the commonality between this function and append_dep
auto SourceFile::make(fs::path const &root, fs::path const &parent) noexcept
    -> Result<SourceFile, SourceFileErr> {
  using Ok = Result<SourceFile, SourceFileErr>::Ok;
  using Err = Result<SourceFile, SourceFileErr>::Err;

  auto file = File(root, File::READ);
  if (!file)
    return Err(FileDoesNotExist(root, parent));

  auto maybe_file_content = SourceFile::get_file_content(file);
  if (maybe_file_content == decltype(maybe_file_content)::ERR) {
    return Err(maybe_file_content.err());
  }

  auto &&[fcontent, _, fsize] = maybe_file_content.get();
  auto maybe_m = M::make();
  if (maybe_m == decltype(maybe_m)::ERR)
    return Err(maybe_m.err());
  auto m = maybe_m.get();

  auto const root_idx = m.num_files;
  m.num_files++; // this could cause some off by 1 errors when error reported
  auto &&[start, end] = m.append_path(root);
  auto hash_fut = std::async(std::launch::async, [fcontent, fsize]() {
    return fnv1a(fsize, fcontent);
  });
  auto const ftype = SourceFile::determine_file_type(root.extension());
  if (ftype == SourceFile::HEADER) {
    auto constexpr potential_extensions = array<string_view, 2>{{".cpp", ".c"}};
    auto const potential_impl = (root.parent_path() / root.stem()).string();
    for (auto const &potential_extension : potential_extensions) {
      auto const possible_path =
          fs::path(potential_impl + potential_extension.data());
      if (fs::exists(possible_path)) {
        if (auto m_error = m.append_dep(possible_path, root_idx);
            m_error == decltype(m_error)::ERR) {
          return Err(m_error.get());
        }
      }
    }
    // if those fail then it's probably a HOL, maybe we could report that as a
    // warning(?)
  }

  // write down the path name
  m.files[root_idx].start = start;
  m.files[root_idx].end = end;

  // append type
  m.types[root_idx] = ftype;

  // analyze the deps of this file
  auto constexpr include_prefix = string_view{"#include"};
  auto const potential_include_dirs = get_include_paths();

  // TODO: also have a flag for checking if we're in a comment or not
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
        ++ch;
        auto const *end_of_include_string = ch;
        while (*end_of_include_string != 0 && *end_of_include_string != '"') {
          ++end_of_include_string;
        }

        if (*end_of_include_string == 0)
          return Err(NonTerminatedString(root));

        auto const include_string_size = end_of_include_string - ch;
        if (include_string_size == 0)
          return Err(EmptyFileName(root));

        auto const include_file =
            fs::path(string_view{ch, end_of_include_string});

        if (include_file.stem() == root.stem()) {
          auto const include_f_ext =
              SourceFile::determine_file_type(include_file.extension());
          auto const path_ext =
              SourceFile::determine_file_type(root.extension());
          if (include_f_ext == HEADER && path_ext == IMPL) {
            continue; // ignore this path
          }
        }

        auto const dep_path = root.parent_path() / include_file;

        if (auto m_error = m.append_dep(dep_path, root_idx); m_error) {
          return Err(m_error.get());
        }

      } break;
      case '<': {
        // TODO: global module include
      } break;
      default:
        std::cerr << "unknown char [" << *ch << "]\n";
        // TODO: return malformed #include directive
        break;
      }
    } else {
      /* TODO: to speed things up we can try skipping ws, as well as like
       * continuous words + symbols */
    }
  }

  // get the hash last
  m.hashes[root_idx] = hash_fut.get();

  return Ok(std::move(m));
}

auto SourceFile::display(std::ostream &out,
                         unsigned int const depth) const noexcept -> void {
  out << "All string = [" << string_view{m.all_paths.buffer, m.all_paths.size}
      << "]\n";
  out.flush();
  display_impl(out, depth, 0);
}

auto SourceFile::display_impl(std::ostream &out, unsigned int const depth,
                              unsigned int const idx) const noexcept -> void {

  auto const indents = [](auto const depth) -> string {
    auto res = string(depth, '\t');
    return res;
  }(depth);

  out << indents << "{\n";
  out << indents << "\"type\":\"";
  switch (m.types[idx]) {
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

  out << indents << "\"path\":\""
      << string_view{m.all_paths.buffer + m.files[idx].start,
                     m.all_paths.buffer + m.files[idx].end}
      << "\",\n";

  out << std::hex << indents << "\"hash\":" << m.hashes[idx] << ",\n";

  out << indents << "\"deps\":[\n";

  for (auto const dep_idx : m.deps[idx]) {
    display_impl(out, depth + 1, dep_idx);
  }

  out << indents << "]\n";

  out << indents << "}\n";
}

/*
auto SourceFile::serialize(fs::path const &path) const noexcept -> void {
  auto outfile = File(path, File::WRITE | File::BINARY);
  if (outfile == nullptr) {
    return;
  }

  serialize_impl(outfile);
  outfile.flush();
}

auto SourceFile::deserialize(fs::path const &path) noexcept -> SourceFile {
  auto file = File(path, File::READ | File::BINARY);
  if (file == nullptr) {
    std::cerr << "unable to open serialization file [" << path << "]\n";
    std::terminate();
  }

  return SourceFile::deserialize_impl(file);
}
*/

auto SourceFile::get_file_content(FILE *file) noexcept
    -> Result<OwnedString, SourceFileErr> {
  using Ok = Result<OwnedString, SourceFileErr>::Ok;
  using Err = Result<OwnedString, SourceFileErr>::Err;
  if (fseek(file, 0, SEEK_END) == -1)
    return Err(CFileAPI(strerror(errno)));

  auto const _fsize = ftell(file);
  if (_fsize == -1)
    return Err(CFileAPI(strerror(errno)));

  auto fsize = static_cast<size_t>(_fsize);
  rewind(file);

  auto *fcontent = (char *)malloc(sizeof(char) * fsize + 1);
  if (fcontent == nullptr)
    return Err(CFileAPI(strerror(errno)));

  if (auto const amount_read = fread(fcontent, sizeof(char), fsize, file);
      amount_read != fsize) {
    free(fcontent);
    return Err(CFileAPI(strerror(errno)));
  }
  fcontent[fsize] = 0;
  return Ok(OwnedString(fcontent, fsize));
}

/*
auto SourceFile::serialize_impl(File &file) const noexcept -> void {
  file.write(&m.type, sizeof(decltype(M::type)), 1);

  file.write(&m.hash, sizeof(decltype(M::hash)), 1);

  // i hope this doesn't alloc that'd be annoying
  auto const path_len = m.path.string().size();
  file.write(&path_len, sizeof(decltype(path_len)), 1);

  file.write(m.path.c_str(), sizeof(char), path_len);

  auto const deps_size = m.deps.size();
  file.write(&deps_size, sizeof(decltype(M::deps.size())), 1);

  for (auto const &dep : m.deps) {
    dep->serialize_impl(file);
  }
}

auto SourceFile::deserialize_impl(File &file) noexcept -> SourceFile {
  auto sf = SourceFile();
  file.read(&sf.m.type, sizeof(decltype(M::type)), 1);

  file.read(&sf.m.hash, sizeof(decltype(M::hash)), 1);

  auto string_len = decltype(M::path.string().size()){};
  file.read(&string_len, sizeof(decltype(string_len)), 1);
  auto *buffer = (char *)malloc(string_len + 1);
  file.read(buffer, sizeof(char), string_len);
  buffer[string_len] = 0;
  sf.m.path = fs::path(buffer);
  free(buffer);

  auto num_deps = decltype(M::deps.size()){};
  file.read(&num_deps, sizeof(num_deps), 1);
  sf.m.deps.reserve(num_deps);

  for (auto i = decltype(num_deps){}; i < num_deps; ++i)
    sf.m.deps.emplace_back(new SourceFile(deserialize_impl(file)));

  return sf;
}
*/

#if false
// sort of a thread pool like structure that is just for compiling
struct CompilationPool final {
  CompilationPool(Module const &mod,
                  size_t num_threads = std::thread::hardware_concurrency() -
                                       1) noexcept
      : mod(mod) {
    workers.reserve(num_threads);
    remaining_tasks.reserve(8);
  }

  ~CompilationPool() noexcept = default;

  auto add_task(SourceFile const *) noexcept -> void;

  auto run() noexcept -> void;

  auto busy() noexcept -> bool;

  auto get() noexcept -> string;

  CompilationPool() = delete;
  CompilationPool(CompilationPool &&) = delete;
  CompilationPool &operator=(CompilationPool &&) = delete;
  CompilationPool(CompilationPool const &) = delete;
  CompilationPool &operator=(CompilationPool const &) = delete;

private:
  auto _thread_loop() noexcept -> void;

  vector<std::thread> workers;
  vector<fs::path> remaining_tasks;
  std::mutex queue_mtx;

  string result;
  std::mutex result_mtx;

  Module const &mod;
};

auto CompilationPool::run() noexcept -> void {
  for (auto i = 0; i < workers.capacity(); ++i)
    workers.emplace_back([this]() { _thread_loop(); });
}

auto CompilationPool::add_task(SourceFile const *sf) noexcept -> void {
  remaining_tasks.push_back(sf->path());
  for (auto const &dep : sf->deps())
    add_task(dep);
}

auto CompilationPool::busy() noexcept -> bool {
  auto pool_busy = true;
  {
    auto lock = std::unique_lock(queue_mtx);
    pool_busy = !remaining_tasks.empty();
  }
  std::this_thread::sleep_for(std::chrono::nanoseconds(100));
  return pool_busy;
}

auto CompilationPool::_thread_loop() noexcept -> void {
  while (true) {
    auto guard = std::unique_lock(queue_mtx);
    if (remaining_tasks.empty())
      return;

    auto this_path = remaining_tasks.back();
    remaining_tasks.pop_back();
    guard.unlock();

    if (SourceFile::determine_file_type(this_path.extension()) ==
        SourceFile::HEADER) {
      continue;
    }

    auto const invoked_command = std::format(
        "{} -c {} -o {}/{}.o/{}.o", mod.compiler(), this_path.c_str(),
        mod.install_dir(), mod.name(), this_path.stem().c_str());
    std::cout << "[" << invoked_command << "]\n";
    std::cout.flush();
    auto const res = system(invoked_command.c_str());
    if (res == 0) {
      auto res_lock = std::unique_lock(result_mtx);
      result += std::format("{}/{}.o/{}.o ", mod.install_dir(), mod.name(),
                            this_path.stem().c_str());
    }
  }
}

auto CompilationPool::get() noexcept -> string {
  while (busy()) {
    /* wait */
  }

  // wait for all jobs to finish after we know everything has been queued
  // this also cleans up all of the threads, so in the destructor we don't
  // need to call join again
  for (auto &worker : workers)
    worker.join();

  return result;
}

struct Compiler final {
  [[nodiscard]]
  static auto compile(Module const &mod, SourceFile const *sf) noexcept
      -> string {
    auto res = string();

    auto tp = CompilationPool(mod);
    tp.add_task(sf);
    tp.run();
    res = tp.get();

    return res;
  }
};
#endif

auto install_exe(lua_State *state) -> int {
  using Result = Result<SourceFile, SourceFileErr>;
  auto num_args = lua_gettop(state);
  if (num_args != 1) {
    lua_pushstring(state, "Too many arguments.");
    return lua_error(state);
  }

  LUA_ASSERT_FORMAT(state, ret_t, lua_type(state, -1), LUA_TTABLE,
                    "Expected type of argument to `install_exe` to be of type "
                    "table, found [%s]",
                    lua_typename(state, ret_t));

  auto maybe_main_mod = Module::make(Module::EXE, state);
  if (!maybe_main_mod.ok()) {
    auto const msg = std::visit([](auto &&e) { return e.error() + '\n'; },
                                maybe_main_mod.err());

    lua_pushstring(state, msg.c_str());
    return lua_error(state);
  }

  auto main_mod = maybe_main_mod.get();
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

  auto maybe_exe_root = SourceFile::make(main_mod.root());
  switch (maybe_exe_root) {
  case Result::OK: {
    auto exe_root = maybe_exe_root.get();
    exe_root.display(std::cout);
    std::cout.flush();
    return 0;

    /*
    // TODO: there is an error where if you have the following situation
    //   A.h/cpp
    //  /       \
    // B.h/cpp   C.h/cpp
    // \        /
    //  main.cpp
    // then A.cpp will be compiled twice, causing a linking error
    // the way to fix this is to order the dep tree in topological order
    // i.e. we need to topologically sort the dep tree so that it's linearized
    // we also need to reverse the tree to make caching files actually work
    // that is when a file is changed we only recompile all of the dependent
    // files instead of the entire project compile the objects :)
    auto const actually_compiled_files = Compiler::compile(main_mod,
    &exe_root);

    // because of the format of `actually_compiled_files` for the best
    // formatting of the command there shouldn't be a space between it and the
    // -o
    auto const invoked_command = std::format(
        "{} {}-o {}/{}", main_mod.compiler(), actually_compiled_files,
        main_mod.install_dir(), main_mod.name());

    std::cout << "[" << invoked_command << "]\n";
    std::cout.flush();

    if (system(invoked_command.c_str()) != 0) {
      lua_pushfstring(state, "Error compiling [%s]", invoked_command.c_str());
      return lua_error(state);
    } else {
      return 0;
    }
    */
  } break;
  case Result::ERR: {
    auto const msg = std::visit([](auto &&e) { return e.error() + '\n'; },
                                maybe_exe_root.err());

    lua_pushstring(state, msg.c_str());
    return lua_error(state);
  } break;
  }
}

#if false
auto install_static(lua_State *state) -> int {
  auto const num_args = lua_gettop(state);
  if (num_args != 1) {
    lua_pushstring(state, "Too many arguments");
    return lua_error(state);
  }

  LUA_ASSERT_FORMAT(state, ret_t, lua_type(state, -1), LUA_TTABLE,
                    "Expected type of argument to `install_static` "
                    "to be of type table, found [%s]",
                    lua_typename(state, ret_t));

  auto maybe_static_mod = Module::make(Module::STATIC, state);
  if (!maybe_static_mod.ok()) {
    auto const err_msg =
        std::visit([](auto &&e) { return e.error(); }, maybe_static_mod.err());
    lua_pushstring(state, err_msg.c_str());
    return lua_error(state);
  }

  auto static_mod = maybe_static_mod.get();

  auto ec = std::error_code{};
  if (fs::create_directories(fs::path(
          std::format("{}/{}.o", static_mod.install_dir(), static_mod.name())));
      ec) {
    std::cerr << ec.message() << '\n';
    lua_pushstring(state, "Unable to create directory");
    return lua_error(state);
  }
  ec.clear();

  auto maybe_static_root = SourceFile::make(static_mod.root());
  switch (maybe_static_root) {
  case decltype(maybe_static_root)::OK: {
    auto static_root = maybe_static_root.get();
    auto compiled_files = Compiler::compile(static_mod, &static_root);
    auto const invoked_command =
        std::format("ar crs {}/lib{}.a {}", static_mod.install_dir(),
                    static_mod.name(), compiled_files);
    std::cout << '[' << invoked_command << "]\n";
    std::cout.flush();
    if (system(invoked_command.c_str()) != 0) {
      lua_pushfstring(state, "Error compiling [%s]", invoked_command.c_str());
      return lua_error(state);
    } else {
      return 0;
    }
  } break;
  case decltype(maybe_static_root)::ERR:
    auto const msg =
        std::visit([](auto &&e) { return e.error(); }, maybe_static_root.err());
    lua_pushstring(state, msg.c_str());
    return lua_error(state);
  }

  return 0;
}
#endif

auto run(lua_State *L) noexcept -> int {
  auto const num_args = lua_gettop(L);
  if (num_args != 1) {
    lua_pushstring(L, "Too many args to function run");
    return lua_error(L);
  }

  lua_getfield(L, -1, "path");
  LUA_ASSERT(L, lua_type(L, -1), LUA_TSTRING,
             "Expected type of exe.path to be string [in function Run]");
  auto const exe_path = string_view(lua_tolstring(L, -1, nullptr));

  lua_getfield(L, -2, "args");
  switch (auto t = lua_type(L, -1)) {
  case LUA_TNIL:
    // nothing to do either type is explicitly nil, or field is undefined so
    // which is fine bc it's an optional field
    break;
  case LUA_TTABLE:
    // TODO: concatinate all these strings
    break;
  default:
    lua_pushfstring(
        L,
        "Expected type of exe.args to either be `nil` "
        "(undefined) or a table (array), found [%s] [in function Run]",
        lua_typename(L, t));
    return lua_error(L);
  }

  std::cout << "[" << exe_path << "]\n";
  std::cout.flush();

  system(exe_path.data());

  lua_pushnil(L);
  return 1;
}

// TODO: finish this function
auto link_static(lua_State *state) noexcept -> int {
  LUA_ASSERT_FORMAT(state, num_args, lua_gettop(state), 3,
                    "Expected 3 arguments to the link static function '[to]"
                    "[from][args]?', found [%d] arguments",
                    num_args);
  for (int i = -1; i >= -3; --i) {
    LUA_ASSERT_FORMAT(state, arg_t, lua_type(state, i), LUA_TTABLE,
                      "Expected type of argument to be table, found [%s]",
                      lua_typename(state, arg_t));
  }

  if (true) {
    lua_pushstring(state, "function is not fully implimented :)");
    return lua_error(state);
  }

  return 0;
}
} // namespace

auto clang(lua_State *state) noexcept -> int {
  auto const num_args = lua_gettop(state);
  if (num_args != 1) {
    lua_pushstring(state, "Expected one argument to the clang function");
    return lua_error(state);
  }
  auto constexpr compiler_field = string_view{"clang++"};
  auto constexpr opt_level = string_view{"O2"};
  auto constexpr warnings = array<string_view, 3>{{
      string_view{"Wall"},
      string_view{"Wconversion"},
      string_view{"Wpedantic"},
  }};

  // check that the input argument is the right type
  LUA_ASSERT(state, lua_type(state, -1), LUA_TTABLE,
             "Expected type passed into clang function to be a table");

  auto const arg_idx = lua_absindex(state, -1);

  auto const path_var = std::getenv("PATH");
  if (path_var == nullptr) {
    lua_pushstring(state, "Envroinment variable PATH not found, you're on your "
                          "own with this one :)");
    return lua_error(state);
  }

  // this could probably be optimized
  auto string_fut =
      std::async(std::launch::async, [path_var, compiler_field]() -> fs::path {
        // TODO: change this to ';' when on windows platforms :)
        auto constexpr path_separator = ':';
        auto i = 0;

        auto path_view = string_view{};
        auto const *prev_path_end = path_var;
        auto possible_path = fs::path();
        while (path_var[i] != 0) {
          // find next ':'
          while (path_var[i] != 0 && path_var[i] != path_separator) {
            ++i;
          }
          // i think this hack might not read the last variable in PATH (?)
          if (path_var[i] == 0) {
            break;
          }
          path_view = string_view{prev_path_end, &path_var[i]};
          possible_path = fs::path(path_view) / compiler_field;

          if (fs::exists(possible_path)) {
            return possible_path;
          }
          ++i;
          prev_path_end = &path_var[i];
        }

        return fs::path();
      });

  lua_createtable(state, 0, 3); // tbl
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

  lua_createtable(state, 0, 0);

  lua_pushnil(state);
  for (auto i = lua_Integer{1}; lua_next(state, arg_idx) != 0; ++i) {
    switch (lua_type(state, -2)) {
    case LUA_TNUMBER: {
      LUA_ASSERT(state, lua_type(state, -1), LUA_TSTRING,
                 "Expected value type to be a string, found something else in "
                 "the clang argument");
      lua_seti(state, -3,
               i); // treat the value as just being passed to the function
    } break;
    case LUA_TSTRING: {
      LUA_ASSERT(state, lua_type(state, -1), LUA_TSTRING,
                 "Expected value type to be a string, found something else in "
                 "the clang argument");
      // combine both the key and value into the argument
      lua_pushfstring(state, "-%s=%s", lua_tolstring(state, -2, nullptr),
                      lua_tolstring(state, -1, nullptr));
      lua_seti(state, -4, i);
      lua_pop(state, 1); // pop the value from the stack
    } break;
    default:
      lua_pushstring(
          state,
          "While *yes* key's into a table can have any type, please refrain "
          "from using anything other than a number (i.e. passing an array), or "
          "a string for a key+value pair item, or a combination of the two");
      return lua_error(state);
    }
  }

  lua_setfield(state, ret_tbl_idx, "opt_args");

  // wait until the very end to let the async function run the longest, idk if
  // this is a good thing i'm bad with async stuff
  auto const path_to_compiler = string_fut.get();
  if (path_to_compiler == fs::path()) {
    lua_pushstring(state, "Unable to find clang++ binary.");
    return lua_error(state);
  }
  lua_pushstring(state, path_to_compiler.c_str());
  // lua_pushstring(state, compiler_field.data());
  lua_setfield(state, ret_tbl_idx, "compiler");

  return 1;
}

auto make_builder_obj(lua_State *state,
                      std::string_view const builder_obj) noexcept -> void {
  lua_createtable(state, 0, 2);

  lua_pushcfunction(state, install_exe);
  lua_setfield(state, -2, "install_exe");

#if false
  lua_pushcfunction(state, install_static);
  lua_setfield(state, -2, "install_static");
#endif

  lua_pushcfunction(state, link_static);
  lua_setfield(state, -2, "link_static");

  // TODO: add the functions install_dynamic
}

auto make_runner_obj(lua_State *state,
                     std::string_view const runner_obj) noexcept -> void {
  lua_createtable(state, 0, 1);

  lua_pushcfunction(state, run);
  lua_setfield(state, -2, "run");
}
} // namespace luamake_builtins
#undef LUA_ASSERT
