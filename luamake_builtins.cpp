#include "luamake_builtins.hpp"

#include "common.hpp"
#include "luamake_error.hpp"

extern "C" {
#include "lua/lua.h"
}

#include <algorithm>
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

using uint = unsigned int;

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

constexpr auto skippable(char const ch) noexcept -> bool {
  switch (ch) {
  case '#':
    [[fallthrough]];
  case '"':
    [[fallthrough]];
  case '/':
    return false;
  default:
    return true;
  }
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

OwnedString::OwnedString(char *buffer, size_t size) noexcept
    : buffer(buffer), size(0), capacity(size) {}
constexpr OwnedString::OwnedString() noexcept
    : buffer(nullptr), size(0), capacity(0) {}

constexpr OwnedString::OwnedString(OwnedString &&that) noexcept
    : buffer(that.buffer), size(that.size), capacity(that.capacity) {
  that.buffer = nullptr;
  that.size = 0;
  that.capacity = 0;
}

constexpr auto OwnedString::operator=(OwnedString &&that) noexcept
    -> OwnedString & {
  buffer = that.buffer;
  size = that.size;
  capacity = that.capacity;

  that.buffer = nullptr;
  that.size = 0;
  that.capacity = 0;
  return *this;
}

OwnedString::~OwnedString() noexcept {
  if (buffer != nullptr)
    free((void *)buffer);
}

auto OwnedString::append(string &&str) noexcept -> void {
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

struct StringViews final {
  unsigned int start;
  unsigned int end;
};

struct FixedString final {
  char const *buffer;
  size_t size;

  explicit FixedString(char const *buffer, size_t size) noexcept;
  constexpr FixedString() noexcept;

  constexpr FixedString(FixedString &&that) noexcept;

  constexpr auto operator=(FixedString &&that) noexcept -> FixedString &;

  ~FixedString() noexcept;

  FixedString(FixedString const &) = delete;
  FixedString &operator=(FixedString const &) = delete;
};

FixedString::FixedString(char const *buffer, size_t size) noexcept
    : buffer(buffer), size(size) {}

constexpr FixedString::FixedString() noexcept : buffer(nullptr), size(0) {}

constexpr FixedString::FixedString(FixedString &&that) noexcept
    : buffer(that.buffer), size(that.size) {
  that.buffer = nullptr;
  that.size = 0;
}

constexpr auto FixedString::operator=(FixedString &&that) noexcept
    -> FixedString & {
  buffer = that.buffer;
  size = that.size;

  that.buffer = nullptr;
  that.size = 0;
  return *this;
}

FixedString::~FixedString() noexcept {
  if (buffer != nullptr)
    free((void *)buffer);
}

struct Compiler;
struct CompilationPool;

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
    if (parent == fs::current_path()) {
      return std::format("Attempting to open file [{}] that does not exist.",
                         name.c_str());
    } else {
      return std::format("Attempting to open file [{}] that does not exist, "
                         "depended on by [{}].",
                         name.c_str(), parent.c_str());
    }
  };
};

struct NonTerminatedString final {
  fs::path name;

  explicit NonTerminatedString(fs::path const &fname) noexcept : name(fname) {}

  auto error() const noexcept -> string {
    return std::format(
        "File [{}] contains a non-terminating string in an include path.",
        name.c_str());
  };
};

struct MalformedInclude final {
  fs::path name;

  explicit MalformedInclude(fs::path const &fname) noexcept : name(fname) {}

  auto error() const noexcept -> string {
    return std::format("File [{}] contains a malformed include path",
                       name.c_str());
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

using DepTreeErr =
    std::variant<EmptyFileName, FileDoesNotExist, NonTerminatedString,
                 MalformedInclude, CFileAPI, MemoryAlloc>;

struct MissingField final {
  string_view field_name;
  MissingField(string_view &&field_name) noexcept : field_name(field_name) {}
  auto error() const noexcept -> string {
    return std::format(
        "Required field [{}] could not be found when constructing a module.",
        field_name);
  }
};

struct UnexpectedType final {
  string_view field_name;
  // idk why the lua_typename function needs a lua_State* but fine
  lua_State *state;
  int expected_type;
  int found_type;
  UnexpectedType(string_view &&field_name, lua_State *state, int expected_type,
                 int found_type) noexcept
      : field_name(field_name), state(state), expected_type(expected_type),
        found_type(found_type) {}
  auto error() const noexcept -> string {
    return std::format("Required field [{}] found, but was of type {}, "
                       "expected type {}, when constructing a module.",
                       field_name, lua_typename(state, found_type),
                       lua_typename(state, expected_type));
  }
};

using ModuleErr = std::variant<MissingField, UnexpectedType>;

struct Module final {
  enum Module_t {
    EXE,
    STATIC,
    DYNAMIC,
  };

  static auto make(Module_t type, lua_State *state) noexcept
      -> Result<Module, ModuleErr>;

  auto gen_dep_tree() noexcept -> Opt<DepTreeErr>;

  auto constexpr install_dir() const noexcept -> char const * {
    return m.install_dir;
  }

  auto constexpr name() const noexcept -> char const * { return m.name; }

  auto roots() const noexcept -> vector<fs::path> { return m.roots; }

  auto compiler() const noexcept -> std::string { return m.compiler; }

  Module(Module &&) = default;
  Module &operator=(Module &&) = default;
  ~Module() noexcept = default;

  Module(Module const &) = delete;
  Module &operator=(Module const &) = delete;

private:
  static auto parse_compiler_table(lua_State *state) -> string;

  // this is kinda stupid i'm not gonna lie, but this is the only
  // way i can think to have DepTree be able to reference Module and vice versa
  // without having to worry about pointer indirection
  struct DepTree final {
    enum SourceFile_t : unsigned char {
      IMPL,
      HEADER,
      SYSTEM,
      MISC,
    };

    DepTree(DepTree const &) = delete;
    DepTree &operator=(DepTree const &) = delete;

    DepTree(DepTree &&) = default;
    DepTree &operator=(DepTree &&) = default;

    ~DepTree() noexcept = default;

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
    // a parallel array for all of the source files
    // NOTE: this could be pushed further, and we could have a
    // memory allocator as a part of this struct, then just
    // clearing the memory allocator would act as the destructor
    // TODO: if performance becomes an issue, it might be good to switch this to
    // a hash set for the `find` function
    struct M final {
      size_t num_files;
      size_t cap_files;
      OwnedString all_paths;
      std::unique_ptr<SourceFile_t[]> types;
      std::unique_ptr<StringViews[]> files;
      std::unique_ptr<vector<unsigned int>[]> deps;
      std::unique_ptr<size_t[]> hashes;

      [[nodiscard]]
      static auto make(size_t const num_files = 8) noexcept
          -> Result<M, DepTreeErr>;
      [[nodiscard]]
      auto append_path(fs::path const &) noexcept
          -> pair<unsigned int, unsigned int>;
      [[nodiscard]]
      auto append_dep(fs::path const &root, size_t const parent_idx) noexcept
          -> Opt<DepTreeErr>;
      [[nodiscard]]
      auto get_path(size_t const) const noexcept -> fs::path;
      [[nodiscard]]
      auto find(string_view const string) const noexcept
          -> std::tuple<bool, unsigned int, unsigned int>;
    } m;

    // basically making the assumption that a project isn't gonna have
    // size_t.max files in it, idk if that's even physically possible
    // so this *seems like* a valid assumption
    static constexpr auto ROOT_IDX = static_cast<size_t>(-1);

    // this should probably returned a FixedString, we don't need the size and
    // capacity
    static auto get_file_content(FILE *file) noexcept
        -> Result<FixedString, DepTreeErr>;

    auto display_impl(std::ostream &out, unsigned int const depth,
                      unsigned int const idx) const noexcept -> void;
    /*
    auto serialize_impl(File &file) const noexcept -> void;

    static auto deserialize_impl(File &file) noexcept -> SourceFile;
    */

    DepTree() = default;
    DepTree(DepTree::M &&m) noexcept : m(std::move(m)) {}
    friend Result<DepTree, DepTreeErr>;
    friend Compiler;
    friend CompilationPool;
    friend Module;
  };

  struct M {
    Module_t type;
    // it *might* be a cool idea to have this as a union of
    // vector<fs::path> and fs::path for better domain modeling, but
    // unions are a bit of a pain to work with in c++
    DepTree tree;
    vector<fs::path> roots;
    vector<fs::path> includes;
    vector<fs::path> linking;
    std::string compiler;
    char const *name;
    char const *install_dir;

    [[remove]]
    auto display(std::ostream &) const noexcept -> void;
  } m;

  Module(M &&m) noexcept : m(std::move(m)) {}
  Module() noexcept = default;
  friend Result<Module, ModuleErr>;
  friend CompilationPool;
  friend Compiler;
};

auto Module::DepTree::M::make(size_t const num_files) noexcept
    -> Result<DepTree::M, DepTreeErr> {
  using Ok = Result<DepTree::M, DepTreeErr>::Ok;
  using Err = Result<DepTree::M, DepTreeErr>::Err;

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

auto Module::DepTree::M::append_path(fs::path const &path) noexcept
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

auto Module::DepTree::M::append_dep(fs::path const &dep,
                                    size_t const parent_idx) noexcept
    -> Opt<DepTreeErr> {
  using Opt = Opt<DepTreeErr>;
  using Err = Opt::Err;

  auto file = File(dep, File::READ);
  if (!file)
    return Err(FileDoesNotExist(dep, get_path(parent_idx)));

  auto maybe_file_content = DepTree::get_file_content(file);
  if (maybe_file_content == decltype(maybe_file_content)::ERR) {
    return maybe_file_content;
  }

  auto &&[fcontent, fsize] = maybe_file_content.get();

  auto const root_idx = num_files;
  assert(root_idx < std::numeric_limits<unsigned int>::max());
  if (parent_idx != ROOT_IDX)
    deps[parent_idx].push_back(static_cast<unsigned int>(root_idx));
  ++num_files;

  auto &&[start, end] = append_path(dep);
  auto hash_fut = std::async(std::launch::async, [fcontent, fsize]() {
    return fnv1a(fsize, fcontent);
  });
  auto const ftype = DepTree::determine_file_type(dep.extension());
  if (ftype == SourceFile_t::HEADER) {
    auto constexpr potential_extensions = array<string_view, 2>{{".cpp", ".c"}};
    auto const potential_impl = (dep.parent_path() / dep.stem()).string();
    for (auto const &potential_extension : potential_extensions) {
      auto const possible_path =
          fs::path(potential_impl + potential_extension.data());
      if (fs::exists(possible_path)) {
        if (auto m_error = append_dep(possible_path, root_idx); !m_error.ok()) {
          return m_error;
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

  for (auto const *ch = fcontent; *ch != 0;) {
    switch (*ch) {
    case '#': // possible include
    {
      if (strncmp(ch, include_prefix.data(), include_prefix.size()) == 0) {
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
            return Err(NonTerminatedString(dep));

          auto const include_string_size = end_of_include_string - ch;
          if (include_string_size == 0)
            return Err(EmptyFileName(dep));

          auto const include_file =
              fs::path(string_view{ch, end_of_include_string});

          if (include_file.stem() == dep.stem()) {
            auto const include_f_ext =
                DepTree::determine_file_type(include_file.extension());
            auto const path_ext = DepTree::determine_file_type(dep.extension());
            if (include_f_ext == HEADER && path_ext == IMPL) {
              continue; // ignore this path
            }
          }

          // TODO: this is where we potentially have to look in different paths
          // if the file doesn't exist in the local dir
          // could rename this to deps_dep lol
          auto const dep_path = dep.parent_path() / include_file;

          if (auto m_error = append_dep(dep_path, root_idx); !m_error.ok())
            return m_error;
        } break;
        case '<': {
          // TODO global include
        } break;
        default:
          return Err(MalformedInclude(dep));
        }
      } else {
        ++ch;
      }
    } break;
    case '/': // possible comment
      ++ch;
      switch (*ch) {
      case 0:
        return Err(NonTerminatedString(
            dep)); // this should be a different error type i'm just tired
      case '/':    // advance to end of line
        ++ch;
        while (*ch != 0 && *ch != '\n')
          ++ch;
        if (*ch != 0)
          ++ch; // ch (should) == '\n';
        break;
      case '*': { // advance until */
        ++ch;
        auto found_end = false;
        while (!found_end) {
          while (*ch != 0 && *ch != '*')
            ++ch;
          switch (*ch) {
          case 0:
            return Err(NonTerminatedString(dep)); // not right error i'm tired
          case '*': // check that next char is also a '/'
            ++ch;
            if (*ch != 0 && *ch == '/')
              found_end = true;
            break;
          default:
            ++ch;
            break;
          }
        }
      } break;
      default: // probably a part of a math eq, or just a malformed file, idk
               // ignoring :)
        break;
      }
      break;
    case '"': // string to move over
      while (*ch != 0 && *ch != '"') {
        ++ch;
      }
      if (*ch == 0) {
        return Err(NonTerminatedString(dep));
      } else {
        ++ch; // *ch (should) == '"'
      }
      break;
    default:
      while (*ch != 0 && skippable(*ch)) {
        ++ch;
      }
      break;
    }
  }

  hashes[root_idx] = hash_fut.get();

  return Opt();
}

auto Module::DepTree::M::get_path(size_t const idx) const noexcept -> fs::path {
  if (idx == ROOT_IDX)
    return fs::current_path();
  auto &&[start, end] = files[idx];
  return fs::path(all_paths.buffer + start, all_paths.buffer + end);
}

// this could (and probably should (if possible)) be rewritten to use the files
// array(?)
auto Module::DepTree::M::find(string_view const path) const noexcept
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

auto Module::DepTree::display(std::ostream &out,
                              unsigned int const depth) const noexcept -> void {
  out << "All string = [" << string_view{m.all_paths.buffer, m.all_paths.size}
      << "]\n";
  out.flush();
  display_impl(out, depth, 0);
}

auto Module::DepTree::display_impl(std::ostream &out, unsigned int const depth,
                                   unsigned int const idx) const noexcept
    -> void {

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

auto Module::DepTree::get_file_content(FILE *file) noexcept
    -> Result<FixedString, DepTreeErr> {
  using Ok = decltype(get_file_content(file))::Ok;
  using Err = decltype(get_file_content(file))::Err;
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
  return Ok(FixedString(fcontent, fsize));
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

auto Module::M::display(std::ostream &out) const noexcept -> void {
  auto _display = [&](auto x) { out << x; };

  out << "type = ";
  switch (type) {
  case EXE:
    out << "EXE";
    break;
  case STATIC:
    out << "STATIC";
    break;
  case DYNAMIC:
    out << "DYNAMIC";
    break;
  }

  out << '\n';

  out << "roots = [";
  std::for_each(roots.begin(), roots.end(), _display);
  out << "]\n";

  out << "includes= [";
  std::for_each(includes.begin(), includes.end(), _display);
  out << "]\n";

  out << "linking = [";
  std::for_each(linking.begin(), linking.end(), _display);
  out << "]\n";

  out << "compiler = " << compiler << '\n';
  out << "name = " << name << '\n';
  out << "install_dir = " << install_dir << '\n';
  out.flush();
}

auto Module::make(Module_t type, lua_State *state) noexcept
    -> Result<Module, ModuleErr> {
  using Ok = Result<Module, ModuleErr>::Ok;
  using Err = Result<Module, ModuleErr>::Err;
  auto ret_t = M{type};

  switch (auto const name_t = lua_getfield(state, -1, "name")) {
  case LUA_TSTRING:
    ret_t.name = lua_tolstring(state, -1, nullptr);
    break;
  case LUA_TNIL:
    return Err(MissingField("name"));
  default:
    return Err(UnexpectedType("name", state, LUA_TSTRING, name_t));
  }

  switch (type) {
  case EXE:
    ret_t.roots.reserve(1);
    switch (auto const root_t = lua_getfield(state, -2, "root")) {
    case LUA_TSTRING:
      ret_t.roots.push_back(fs::path(lua_tolstring(state, -1, nullptr)));
      break;
    case LUA_TNIL:
      return Err(MissingField("root"));
    default:
      return Err(UnexpectedType("root", state, LUA_TSTRING, root_t));
    }
    break;
  case STATIC: {
    switch (auto const root_t = lua_getfield(state, -2, "roots")) {
    case LUA_TTABLE: {
      auto const num_roots = lua_rawlen(state, -1);
      auto const roots = lua_absindex(state, -1);
      ret_t.roots.reserve(num_roots);
      for (lua_pushnil(state); lua_next(state, roots) != 0;) {
        if (auto const value_t = lua_type(state, -1); value_t != LUA_TSTRING) {
          return Err(UnexpectedType("roots[i]", state, LUA_TSTRING, value_t));
        }
        ret_t.roots.push_back(lua_tolstring(state, -1, nullptr));
        lua_pop(state, 1);
      }
    } break;
    case LUA_TNIL:
      return Err(MissingField("roots"));
    default:
      return Err(UnexpectedType("roots", state, LUA_TTABLE, root_t));
    }
  } break;
  case DYNAMIC:
    std::cerr << "Not currently implimented\n";
    std::terminate();
    break;
  }

  switch (auto const compiler_t = lua_getfield(state, -3, "compiler")) {
  case LUA_TTABLE:
    ret_t.compiler = Module::parse_compiler_table(state);
    break;
  case LUA_TNIL:
    return Err(MissingField("compiler"));
  default:
    return Err(UnexpectedType("compiler", state, LUA_TTABLE, compiler_t));
  }

  switch (auto const install_dir_t = lua_getfield(state, -4, "install_dir")) {
  case LUA_TSTRING:
    ret_t.install_dir = lua_tolstring(state, -1, nullptr);
    break;
  case LUA_TNIL:
    return Err(MissingField("install_dir"));
  default:
    return Err(
        UnexpectedType("install_dir", state, LUA_TSTRING, install_dir_t));
  }

  // TODO: have an accumulator that tells us how many we have to pop from the
  // stack auto num_pop = int{};
  switch (auto const include_t = lua_getfield(state, -5, "include")) {
  case LUA_TTABLE: {
    auto const len = lua_rawlen(state, -1);
    ret_t.includes.reserve(len);
    auto include = -1;
    for (auto i = 1; i <= len; ++i) {
      switch (auto const value_t = lua_geti(state, include, i)) {
      case LUA_TSTRING:
        ret_t.includes.push_back(lua_tolstring(state, -1, nullptr));
        break;
      default:
        return Err(UnexpectedType("include[i]", state, LUA_TSTRING, value_t));
      }
      --include;
    }
    lua_pop(state, static_cast<int>(len));
  } break;
  case LUA_TNIL:
    break;
  default:
    return Err(UnexpectedType("include", state, LUA_TTABLE, include_t));
  }
  // the compiler command /usr/bin/clang-20 which this goes down into
  // displays into stderr not stdout, so idk we actually have to set up
  // our own pipes to read to and from it :)
  /*
  auto const command_string =
      std::format("{} -v -c -xc++ /dev/null",
                  string_view{ret_t.compiler.data(), ret_t.compiler.find(' ')});

  std::cerr << "running [" << command_string << "]\n";
  auto *child = popen(command_string.c_str(), "r");
  auto constexpr buffer_size = 128 + 1;
  char buffer[buffer_size] = {};
  buffer[buffer_size - 1] = 0;
  for (auto amount_read =
           std::fgets(buffer, sizeof(char) * (buffer_size - 1), child);
       amount_read != 0; amount_read = std::fgets(
                             buffer, sizeof(char) * (buffer_size - 1), child)) {
    std::cerr << "read 32 bytes from child, recieved [" << buffer << "]\n";
  }
  pclose(child);
  */

  switch (auto const linking_t = lua_getfield(state, -6, "linking")) {
  case LUA_TTABLE: {
    auto const len = lua_rawlen(state, -1);
    ret_t.linking.reserve(len);
    auto linking = -1;
    for (auto i = 1; i <= len; ++i) {
      switch (auto const value_t = lua_geti(state, linking, i)) {
      case LUA_TSTRING:
        ret_t.linking.push_back(lua_tolstring(state, -1, nullptr));
        break;
      default:
        return Err(UnexpectedType("linking[i]", state, LUA_TSTRING, value_t));
      }
      --linking;
    }
    lua_pop(state, static_cast<int>(len));
  } break;
  case LUA_TNIL:
    break;
  default:
    return Err(UnexpectedType("linking", state, LUA_TTABLE, linking_t));
  }

  lua_pop(state, 5);

  return Ok(std::move(ret_t));
}

auto Module::gen_dep_tree() noexcept -> Opt<DepTreeErr> {
  using Opt = Opt<DepTreeErr>;

  auto m_m = DepTree::M::make();
  if (m_m == decltype(m_m)::ERR)
    return m_m;

  m.tree = m_m.get();
  for (auto const &root : m.roots) {
    if (auto m_err = m.tree.m.append_dep(root, DepTree::ROOT_IDX);
        m_err == decltype(m_err)::ERR) {
      return m_err;
    }
  }

  return Opt();
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

// sort of a thread pool like structure that is just for compiling
struct CompilationPool final {
  CompilationPool(size_t num_threads) noexcept {
    workers.reserve(num_threads);
    remaining_tasks.reserve(8);
  }

  ~CompilationPool() noexcept = default;

  auto init(Module const *const mod) noexcept -> void;

private:
  // idk probably just have this take a module my const & (?)
  auto add_task(Module::DepTree const &) noexcept -> void;

  auto run() noexcept -> void;

  auto busy() noexcept -> bool;

  auto get() noexcept -> string;

  CompilationPool() = delete;
  CompilationPool(CompilationPool &&) = delete;
  CompilationPool &operator=(CompilationPool &&) = delete;
  CompilationPool(CompilationPool const &) = delete;
  CompilationPool &operator=(CompilationPool const &) = delete;

  auto _thread_loop() noexcept -> void;

  vector<std::thread> workers;
  vector<fs::path> remaining_tasks;
  std::mutex queue_mtx;

  string result;
  std::mutex result_mtx;

  Module const *mod;
  friend Compiler;
};

static auto pool = CompilationPool(std::thread::hardware_concurrency() - 1);

auto CompilationPool::init(Module const *const mod) noexcept -> void {
  this->mod = mod;
}

auto CompilationPool::run() noexcept -> void {
  for (auto i = 0; i < workers.capacity(); ++i)
    workers.emplace_back([this]() { _thread_loop(); });
}

auto CompilationPool::add_task(Module::DepTree const &sf) noexcept -> void {
  // this is a really hacky solution to fix the issues of compiling the same
  // source multiple times, this is probably where that hash set solution would
  // probably make things faster :)
  auto lowest = uint{0};
  for (auto i = size_t{}; i < sf.m.num_files; ++i) {
    if (sf.m.types[i] == Module::DepTree::IMPL &&
        sf.m.files[i].start >= lowest) {
      remaining_tasks.push_back(fs::path(sf.m.get_path(i)));
      lowest = sf.m.files[i].start + 1;
    }
  }
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

    // TODO: see if we can remove this, i think we're only pushing back the IMPL
    // files anyways so there's no need to check this here
    if (Module::DepTree::determine_file_type(this_path.extension()) ==
        Module::DepTree::HEADER) {
      continue;
    }

    auto const invoked_command = std::format(
        "{} -c {} -o {}/{}.o/{}.o", mod->compiler(), this_path.c_str(),
        mod->install_dir(), mod->name(), this_path.stem().c_str());
    std::cout << "[" << invoked_command << "]\n";
    std::cout.flush();
    auto const res = system(invoked_command.c_str());
    if (res == 0) {
      auto res_lock = std::unique_lock(result_mtx);
      result += std::format("{}/{}.o/{}.o ", mod->install_dir(), mod->name(),
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

  mod = nullptr;

  return result;
}

struct Compiler final {
  [[nodiscard]]
  static auto compile(Module const &mod) noexcept -> string {
    auto res = string();

    pool.init(&mod);
    pool.add_task(mod.m.tree);
    pool.run();
    res = pool.get();
    return res;
  }
};

auto install_exe(lua_State *state) noexcept -> int {
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
    auto const msg =
        std::visit([](auto &&e) -> string { return e.error() + '\n'; },
                   *maybe_main_mod.err().get());

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

  if (auto m_err = main_mod.gen_dep_tree(); m_err == decltype(m_err)::ERR) {
    auto const msg =
        std::visit([](auto &&e) -> string { return e.error() + '\n'; },
                   *m_err.err().get());
    lua_pushstring(state, msg.c_str());
    return lua_error(state);
  }

  auto const actually_compiled_files = Compiler::compile(main_mod);

  // because of the format of `actually_compiled_files` for the best
  // formatting of the command there shouldn't be a space between it and the
  // -o
  auto const invoked_command =
      std::format("{} {}-o {}/{}", main_mod.compiler(), actually_compiled_files,
                  main_mod.install_dir(), main_mod.name());

  std::cout << "[" << invoked_command << "]\n";
  std::cout.flush();

  if (system(invoked_command.c_str()) != 0) {
    lua_pushfstring(state, "Error compiling [%s]", invoked_command.c_str());
    return lua_error(state);
  } else {
    return 0;
  }
}

auto install_static(lua_State *state) noexcept -> int {
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
        std::visit([](auto &&e) -> string { return e.error() + '\n'; },
                   *maybe_static_mod.err().get());
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

  if (auto m_err = static_mod.gen_dep_tree(); m_err == decltype(m_err)::ERR) {
    auto const msg = std::visit([](auto &&e) { return e.error() + '\n'; },
                                *m_err.err().get());
    lua_pushstring(state, msg.c_str());
    return lua_error(state);
  }

  auto const compiled_files = Compiler::compile(static_mod);
  auto const invoked_command =
      std::format("ar crs {}/lib{}.a {}", static_mod.install_dir(),
                  static_mod.name(), compiled_files);

  std::cout << '[' << invoked_command << "]\n";
  std::cout.flush();
  if (system(invoked_command.c_str()) != 0) {
    lua_pushstring(
        state, std::format("Error compiling [{}]", invoked_command).c_str());
    return lua_error(state);
  } else {
    return 0;
  }
}

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

  return 0;
}

// TODO: update this function to work with the static_lib.roots
auto link_static(lua_State *state) noexcept -> int {
  LUA_ASSERT_FORMAT(
      state, num_args, lua_gettop(state), 3,
      "Expected 3 arguments to the link static function '[static library]"
      "[exe][args]?', found [%d] arguments",
      num_args);
  for (int i = -1; i >= -3; --i) {
    LUA_ASSERT_FORMAT(state, arg_t, lua_type(state, i), LUA_TTABLE,
                      "Expected type of argument to be table, found [%s]",
                      lua_typename(state, arg_t));
  }

  // going to ignore the args parameter for now, idk what would even go in it
  // tbh
  auto const slib_idx = lua_absindex(state, -3);
  auto const exe_idx = lua_absindex(state, -2);

  // check if there is already an include table
  switch (auto const include_table_t =
              lua_getfield(state, exe_idx, "include")) {
  case LUA_TNIL:
    lua_pop(state, 1);
    lua_createtable(state, 1, 0); // include_tbl
    break;
  case LUA_TTABLE:
    break;
  default:
    lua_pushstring(state,
                   std::format("Found include field in [exe] table, but was "
                               "of incorrect type. Expected [table] found {}",
                               lua_typename(state, include_table_t))
                       .c_str());
    return lua_error(state);
  }
  auto const include_tbl = lua_absindex(state, -1);

  // get slib.roots for the -I flag
  switch (auto const roots_t = lua_getfield(state, slib_idx, "roots")) {
  case LUA_TTABLE:
    break;
  case LUA_TNIL:
    lua_pushstring(
        state,
        "Missing field [static library].roots in the `link_static` function");
    return lua_error(state);
  default:
    lua_pushstring(
        state,
        std::format("In `link_static` function, expected type of field [static "
                    "labrary].roots to be of type [table] found [{}]",
                    lua_typename(state, roots_t))
            .c_str());
    return lua_error(state);
  }
  auto const roots = lua_absindex(state, -1);
  // i'm just assuming that this table is an array, and that the user won't mess
  // that up
  auto init_include_tbl_len = lua_rawlen(state, -2);
  for (lua_pushnil(state); lua_next(state, roots); ++init_include_tbl_len) {
    LUA_ASSERT_FORMAT(
        state, value_t, lua_type(state, -1), LUA_TSTRING,
        "Expected value type in roots table to be a [string], found %s",
        lua_typename(state, value_t));
    auto const path = fs::path(lua_tolstring(state, -1, nullptr));
    lua_pushstring(state, path.parent_path().c_str());
    lua_seti(state, include_tbl,
             static_cast<lua_Integer>(init_include_tbl_len) + 1);
    lua_pop(state, 1); // still need to pop the original value from the stack
  }

  lua_pop(state, 1); // remove the roots table from the top of the stack

  lua_setfield(state, exe_idx, "include");

  // check if there's already a linking table in the object
  switch (auto const linking_tbl_t = lua_getfield(state, exe_idx, "linking")) {
  case LUA_TNIL:
    lua_pop(state, 1);
    lua_createtable(state, 1, 0); // include_tbl
    break;
  case LUA_TTABLE:
    break;
  default:
    lua_pushstring(state,
                   std::format("Found linking field in [exe], but was "
                               "of incorrect type. Expected [table] found [{}]",
                               lua_typename(state, linking_tbl_t))
                       .c_str());
    return lua_error(state);
  }
  // get slib.name and slib.install_dir for adding the archive to be linked
  LUA_ASSERT_FORMAT(
      state, name_t, lua_getfield(state, slib_idx, "name"), LUA_TSTRING,
      "Expected type of [static library].name to be [string] found [%s]",
      lua_typename(state, name_t));
  LUA_ASSERT_FORMAT(
      state, install_dir_t, lua_getfield(state, slib_idx, "install_dir"),
      LUA_TSTRING,
      "Expected type of [static library].root to be [string] found [%s]",
      lua_typename(state, install_dir_t));
  auto const link_format =
      std::format("{}/lib{}.a", lua_tolstring(state, -1, nullptr),
                  lua_tolstring(state, -2, nullptr));
  lua_pop(state, 2);
  lua_pushstring(state, link_format.c_str());

  auto const link_tbl_len = lua_rawlen(state, -2);
  lua_seti(state, -2, static_cast<lua_Integer>(link_tbl_len + 1));

  lua_setfield(state, exe_idx, "linking");
  return 0;
}

auto dump_impl(lua_State *state, unsigned int const depth) noexcept -> void {
  auto const indents = string(depth, '\t');
  switch (auto const type = lua_type(state, -1)) {
  case LUA_TNIL:
    std::cout << "nil";
    break;
  case LUA_TSTRING:
    std::cout << '"' << lua_tolstring(state, -1, nullptr) << '"';
    break;
  case LUA_TNUMBER:
    std::cout << lua_tonumber(state, -1);
    break;
  case LUA_TBOOLEAN:
    std::cout << (lua_toboolean(state, -1) ? "true" : "false");
    break;
  case LUA_TTABLE: {
    std::cout << "{\n";
    auto const tbl_idx = lua_absindex(state, -1);
    for (lua_pushnil(state); lua_next(state, tbl_idx) != 0;) {
      std::cout << indents;
      switch (auto const key_t = lua_type(state, -2)) {
      case LUA_TNUMBER:
        std::cout << '[' << lua_tointeger(state, -2) << ']';
        break;
      case LUA_TSTRING:
        std::cout << '[' << lua_tolstring(state, -2, nullptr) << ']';
        break;
      default:
        std::cout << lua_typename(state, key_t);
        break;
      }

      std::cout << ':';
      dump_impl(state, depth + 1);

      lua_pop(state, 1);
    }
    std::cout << indents << '}';
  } break;
  default:
    std::cout << "Unable to display type of " << lua_typename(state, type);
    break;
  }
  std::cout << '\n';
}
} // namespace

auto dump(lua_State *state) noexcept -> int {
  auto const num_args = lua_gettop(state);
  if (num_args != 2) {
    lua_pushstring(state, std::format("Expected 2 arguments to dump function, "
                                      "[display string][value], found [{}]",
                                      num_args)
                              .c_str());
    return lua_error(state);
  }
  // don't really care about type checking the args
  std::cout << '[' << lua_tolstring(state, -2, nullptr) << "] = ";
  dump_impl(state, 1);
  return 0;
}

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

  // this seems slow, should benchmark it to see if it's causing the massive
  // slow down i'm noticing
  auto string_fut =
      std::async(std::launch::async, [path_var, compiler_field]() -> fs::path {
#if defined(_WIN32)
        auto constexpr path_sep = ';';
#else
        auto constexpr path_sep = ':';
#endif

        auto const *start = path_var;
        auto const *end = start;

        auto ec = std::error_code{};
        while (true) {
          while (*end != 0 && *end != path_sep) {
            ++end;
          }
          switch (*end) {
          case 0: {
            if (fs::exists(fs::path(start, end) / compiler_field, ec)) {
              return fs::path(start, end) / compiler_field;
            } else {
              // unable to find binary
              return fs::path();
            }
          } break;
          case path_sep: {
            if (fs::exists(fs::path(start, end) / compiler_field, ec)) {
              return fs::path(start, end) / compiler_field;
            } else {
              // binary is not is this directory
              start = end + 1; // skip the part_sep
              ++end;
            }
          } break;
          default: {
            return fs::path();
          }
          }
        }
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

  lua_pushcfunction(state, install_static);
  lua_setfield(state, -2, "install_static");

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
