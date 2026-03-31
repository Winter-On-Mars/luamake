#include "luamake_builtins.hpp"

#include "common.hpp"
#include "luamake_file.hpp"
#include "luamake_pre_ir.hpp"
#include "luamake_strings.hpp"
#include "luamake_thread_pool.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <array>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <endian.h>
#include <sys/wait.h>
#include <unistd.h>

#define LUA_ASSERT(L, A, B, ERROR)                                             \
  {                                                                            \
    if ((A) != (B)) {                                                          \
      lua_pushstring((L), (ERROR));                                            \
      return lua_error((L));                                                   \
    }                                                                          \
  }

#define LUA_ASSERT_FORMAT(L, name, A, B, fmt, ...)                             \
  {                                                                            \
    if (auto const name = (A); (name) != (B)) {                                \
      lua_pushfstring((L), fmt, __VA_ARGS__);                                  \
      return lua_error((L));                                                   \
    }                                                                          \
  }

#define LUA_EXPECTED_ARGUMENTS(L, expected_args, fn_name)                      \
  {                                                                            \
    auto const num_args = lua_gettop(L);                                       \
    if (num_args != expected_args) {                                           \
      lua_pushfstring(                                                         \
          L, "Expected " #expected_args " arguments to " #fn_name ", got %d.", \
          num_args);                                                           \
      return lua_error(L);                                                     \
    }                                                                          \
  }

#ifndef PERF_TESTING
#define OS_CALL(command) system(command)
#else
#define OS_CALL(command) 0
#endif

namespace fs = std::filesystem;

// TODO: reorder things in this namespace bc things are kind of all over the
// place
namespace luamake {
namespace {
using std::pair, std::array, std::string, std::string_view, std::vector,
    std::unordered_map, std::unordered_set;

// see the lua docs about lua_type for information about how this function works
// with that
auto constexpr lua_typename(int const type) noexcept -> char const * {
  switch (type) {
  case LUA_TNIL:
    return "LUA_TNIL";
  case LUA_TBOOLEAN:
    return "LUA_TBOOLEAN";
  case LUA_TLIGHTUSERDATA:
    return "LUA_TLIGHTUSERDATA";
  case LUA_TNUMBER:
    return "LUA_TNUMBER";
  case LUA_TSTRING:
    return "LUA_TSTRING";
  case LUA_TTABLE:
    return "LUA_TTABLE";
  case LUA_TFUNCTION:
    return "LUA_TFUNCTION";
  case LUA_TUSERDATA:
    return "LUA_TUSERDATA";
  case LUA_TTHREAD:
    return "LUA_TTHREAD";
  default:
    return "LUA_NONE";
  }
}

auto skip_ws(char const *ch) -> char const * {
  while (*ch != 0) {
    switch (*ch) {
    case ' ':
      [[fallthrough]];
    case '\t':
      [[fallthrough]];
    case '\n':
      [[fallthrough]];
    case '\r': {
      ++ch;
    } break;
    default:
      return ch;
    }
  }
  return ch;
}

// algorithm
// https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function#FNV-1a_hash
auto constexpr fnv1a(std::span<u8 const> const bytes) noexcept -> size_t {
  auto [hash, fnv1a_prime] = []() -> std::pair<size_t, size_t> {
    if constexpr (sizeof(size_t) == 4) {
      return std::make_pair(0x01000193, 0x811c9dc5);
    } else if constexpr (sizeof(size_t) == 8) {
      return std::make_pair(0xcbf29ce484222325, 0x00000100000001b3);
    } else {
      throw;
    }
  }();

  for (size_t i{0}; i < bytes.size(); ++i) {
    hash = hash ^ static_cast<size_t>(bytes[i]);
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

// TODO: collapse all of these to just inherit from std::exception
struct ModuleErr {
  constexpr ModuleErr(string &&message) noexcept : message(message) {}
  virtual ~ModuleErr() = default;
  virtual auto what() const -> string = 0;
  std::string message;
};

struct DepTreeErr {
  virtual ~DepTreeErr() = default;
  virtual auto what() const -> string = 0;
};

struct EmptyFileName final : public DepTreeErr {
  explicit EmptyFileName(fs::path const &path) noexcept : path(path) {}
  auto what() const -> string final {
    return std::format("In [{}] found empty include path.", path.string());
  };
  fs::path path;
};

struct FileDoesNotExist final : public DepTreeErr {
  fs::path name;
  fs::path parent;

  explicit FileDoesNotExist(fs::path const &fname,
                            fs::path const &parent) noexcept
      : name(fname), parent(parent) {}

  auto what() const -> string final {
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

struct NonTerminatedString final : public DepTreeErr {
  fs::path name;

  explicit NonTerminatedString(fs::path const &fname) noexcept : name(fname) {}

  auto what() const -> string final {
    return std::format(
        "File [{}] contains a non-terminating string in an include path.",
        name.c_str());
  };
};

struct MalformedInclude final : public DepTreeErr {
  fs::path name;

  explicit MalformedInclude(fs::path const &fname) noexcept : name(fname) {}

  auto what() const -> string final {
    return std::format("File [{}] contains a malformed include path",
                       name.c_str());
  };
};

struct CAPI final : public ModuleErr, public DepTreeErr {
  explicit CAPI(string &&message) : ModuleErr(std::move(message)) {}
  ~CAPI() = default;
  auto what() const -> string final { return message; }
};

struct MemoryAlloc final : public DepTreeErr {
  char const *fn_name;
  explicit MemoryAlloc(char const *const fn_name) : fn_name(fn_name) {}
  auto what() const -> string final {
    return std::format("Error while allocating memory in function [{}]",
                       fn_name);
  }
};

struct NonTerminatedPreprocessor final : public DepTreeErr {
  fs::path file;
  explicit NonTerminatedPreprocessor(fs::path const &file) noexcept
      : file(file) {}
  auto what() const -> string {
    return std::format("Error while processing the preprocessor in file [{}]",
                       file.c_str());
  }
};

struct MissingField final : public ModuleErr {
  constexpr explicit MissingField(string &&field_name) noexcept
      : ModuleErr(std::move(field_name)) {}
  ~MissingField() final = default;
  auto what() const -> string final {
    return std::format(
        "Required field [{}] could not be found when constructing a module.",
        message);
  }
};

struct UnexpectedType final : public ModuleErr {
  int expected_type;
  int found_type;
  constexpr explicit UnexpectedType(string &&field_name, int expected_type,
                                    int found_type) noexcept
      : ModuleErr(std::move(field_name)), expected_type(expected_type),
        found_type(found_type) {}
  ~UnexpectedType() final = default;
  auto what() const noexcept -> string final {
    return std::format("Required field [{}] found, but was of type {}, "
                       "expected type {}, when constructing a module.",
                       message, lua_typename(found_type),
                       lua_typename(expected_type));
  }
};

struct UnexpectedCharacter final : public ModuleErr {
  constexpr explicit UnexpectedCharacter(string &&ctx, char ch) noexcept
      : ModuleErr(std::move(ctx)), ch(ch) {}
  ~UnexpectedCharacter() final = default;
  auto what() const -> string final {
    return std::format("{} found {}", message, ch);
  }

  char ch;
};

struct MisformattedOutput final : public ModuleErr {
  constexpr explicit MisformattedOutput(string &&command) noexcept
      : ModuleErr(std::move(command)) {}
  ~MisformattedOutput() final = default;
  auto what() const -> string final {
    return std::format("When running command {}, output was not as expected",
                       message);
  }
};

static auto compiler_impl(lua_State *state,
                          string_view const compiler_name) noexcept -> int {
  // idk get better error messages
  LUA_EXPECTED_ARGUMENTS(state, 1, compiler_name);
  LUA_ASSERT(state, lua_type(state, -1), LUA_TTABLE,
             "Expected type passed to compiler function to be a table");
  auto const cc_config_idx = lua_absindex(state, -1);

  auto const path_var = std::getenv("PATH");
  if (path_var == nullptr) {
    lua_pushstring(
        state, "Envroinment variable PATH not found, you're on your own :)");
    return lua_error(state);
  }

  auto compiler_path_fut =
      std::async(std::launch::async, [path_var, compiler_name]() -> fs::path {
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
            if (fs::exists(fs::path(start, end) / compiler_name, ec)) {
              return fs::path(start, end) / compiler_name;
            } else {
              // unable to find binary
              return fs::path();
            }
          } break;
          case path_sep: {
            if (fs::exists(fs::path(start, end) / compiler_name, ec)) {
              return fs::path(start, end) / compiler_name;
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

  lua_createtable(state, 0, 1);
  auto const ret_tbl_idx = lua_absindex(state, -1);

  // opt_args tbl
  lua_createtable(state, 0, 0);
  auto const opt_args = lua_absindex(state, -1);

  lua_pushnil(state);
  for (auto i = lua_Integer{1}; lua_next(state, cc_config_idx) != 0;) {
    switch (lua_type(state, -2)) {
    case LUA_TNUMBER:
      LUA_ASSERT(state, lua_type(state, -1), LUA_TSTRING,
                 "Expected value type in Compiler Config to be either string "
                 "or table");
      // array values are just passed straight to the config
      lua_seti(state, opt_args, i++);
      break;
    case LUA_TSTRING: {
      auto const field_name = lua_tolstring(state, -2, nullptr);
      switch (lua_type(state, -1)) {
      case LUA_TSTRING:
        lua_pushfstring(state, "-%s=%s", field_name,
                        lua_tolstring(state, -1, nullptr));
        lua_seti(state, opt_args, i++);
        lua_pop(state, 1);
        break;
      case LUA_TTABLE: {
        auto const tbl_len = static_cast<lua_Integer>(lua_rawlen(state, -1));
        for (auto j = lua_Integer{1}; j <= tbl_len; ++j) {
          auto const tbl_val_t = lua_geti(state, -1, j);
          LUA_ASSERT(
              state, tbl_val_t, LUA_TSTRING,
              "Expected string in subarray passed to ha%or0\thcrah,.c&h^@cu");
          lua_pushfstring(state, "-%s%s", field_name,
                          lua_tolstring(state, -1, nullptr));
          lua_seti(state, opt_args, i++);
          lua_pop(state, 1);
        }
        lua_pop(state, 1);
      } break;
      default:
        lua_pushfstring(state,
                        "Unexpected value type in table to %s function. "
                        "Expected either string or table value.",
                        compiler_name.data());
        return lua_error(state);
      }
    } break;
    default:
      lua_pushstring(
          state,
          "While *yes* key's into a table can have any type, please refrain "
          "from using anything other than a number (i.e. passing an array), "
          "or "
          "a string for a key+value pair item, or a combination of the two");
      return lua_error(state);
    }
  }

  lua_setfield(state, ret_tbl_idx, "opt_args");

  auto const path_to_compiler = compiler_path_fut.get();
  if (path_to_compiler == fs::path()) {
    lua_pushfstring(state, "Unable to find %s binary", compiler_name.data());
    return lua_error(state);
  }

  lua_pushstring(state, path_to_compiler.c_str());

  lua_setfield(state, ret_tbl_idx, "compiler");

  return 1;
}
} // namespace

namespace builtins {
LakeModules mods = LakeModules();
CLOptions cl_options = CLOptions{};

Module::DepTree::DepTree(size_t const num_files) {
  types = std::make_unique<SourceFile_t[]>(num_files);
  files = std::make_unique<StringViews[]>(num_files);
  deps = std::make_unique<vector<unsigned int>[]>(num_files);
  hashes = std::make_unique<size_t[]>(num_files);

  auto const paths_size = num_files * (sizeof(char) * 15 + 1);
  auto *paths = (char *)malloc(paths_size);
  if (paths == nullptr)
    throw CAPI(strerror(errno));
  std::memset(paths, 0, paths_size);

  all_paths = OwnedString(paths, paths_size);

  this->num_files = 0;
  cap_files = num_files;
}

auto Module::DepTree::append_path(fs::path const &path)
    -> std::pair<bool, StringViews> {
  auto const canonical_path = fs::canonical(path);
  if (auto &&[found, str] = find(canonical_path.c_str()); found) {
    return std::make_pair(true, str);
  }
  auto const start = all_paths.size;
  all_paths.append(canonical_path.string());
  auto const end = all_paths.size;

  if (start >= std::numeric_limits<unsigned int>::max() ||
      end >= std::numeric_limits<unsigned int>::max()) {
    throw std::runtime_error(std::format(
        "Damn you have a lot of path strings, more than [{}], idk see about "
        "opening an issue to change how the indexing work, increasing the size "
        "of the StringViews class?",
        std::numeric_limits<unsigned int>::max()));
  }

  return std::make_pair(false, StringViews{static_cast<unsigned int>(start),
                                           static_cast<unsigned int>(end)});
}

auto Module::append_dep(fs::path const &dep, size_t const parent_idx) -> void {
  if (tree.num_files == tree.cap_files) {
    tree.resize();
  }
  auto const this_idx = tree.num_files;
  if (parent_idx != DepTree::ROOT_IDX)
    tree.deps[parent_idx].push_back(static_cast<unsigned int>(this_idx));
  ++tree.num_files;

  auto &&[found, str] = tree.append_path(dep);
  if (found) // early return if file was already processed
    return;

  auto file = File(dep, File::READ);
  if (!file)
    throw FileDoesNotExist(dep, tree.get_path(parent_idx));

  auto &&[fsize, fcontent] = file.dump_content();

  auto hash_fut = std::async(
      std::launch::async,
      [fsize](u8 const *fcontent) { return fnv1a(std::span(fcontent, fsize)); },
      fcontent.get());
  auto const ftype = DepTree::determine_file_type(dep.extension());
  if (ftype == DepTree::SourceFile_t::HEADER) {
    auto constexpr potential_extensions = array<string_view, 2>{{".cpp", ".c"}};
    auto const potential_impl = (dep.parent_path() / dep.stem()).string();
    for (auto const &potential_extension : potential_extensions) {
      auto const possible_path =
          fs::path(potential_impl + potential_extension.data());
      if (fs::exists(possible_path)) {
        append_dep(possible_path, this_idx);
      }
    }
    // HOL
  }

  tree.types[this_idx] = ftype;
  tree.files[this_idx] = str;

  // this is a big point of failure that needs to be checked to make sure it
  // works
  auto const files_deps = interpreter.interpret(
      std::string_view(reinterpret_cast<char const *>(fcontent.get()), fsize));

#ifdef DEBUG
  std::cout << "Possible includes for " << dep.string() << ": {\n";
  for (auto &&include : files_deps) {
    std::cout << "\t" << include << "\n";
  }
  std::cout << "}\n";
  std::cout.flush();
#endif // DEBUG

  for (auto const &file : files_deps) {
    auto const maybe_file = [&]() -> std::optional<fs::path> {
      for (auto const &include : includes) {
        auto ec = std::error_code{};
        auto const p = fs::canonical(include / file, ec);
        if (ec)
          continue;

        if (DepTree::determine_file_type(p.extension()) !=
                DepTree::SourceFile_t::IMPL &&
            p.parent_path() / p.stem() == dep.parent_path() / dep.stem()) {
          return std::nullopt; // impl file including header file
        }
        return p;
      }
      // TODO: update this to throw
      return std::nullopt;
    }();
    if (maybe_file)
      append_dep(maybe_file.value(), this_idx);
  }

  tree.hashes[this_idx] = hash_fut.get();
}

auto Module::DepTree::get_path(size_t const idx) const noexcept -> fs::path {
  if (idx == ROOT_IDX)
    return fs::current_path();
  auto &&[start, end] = files[idx];
  return fs::path(all_paths.buffer + start, all_paths.buffer + end);
}

// this could (and probably should (if possible)) be rewritten to use the files
// array(?)
auto Module::DepTree::find(string_view const path) const noexcept
    -> std::pair<bool, StringViews> {
  return all_paths.find(path);
}

auto Module::DepTree::resize() noexcept(false) -> void {
  auto const next_cap = 3 * cap_files / 2;
  // these are basic types so we *should* just be able to memmov them
  auto n_types = std::make_unique<SourceFile_t[]>(next_cap);
  std::memmove(n_types.get(), types.get(), sizeof(SourceFile_t) * cap_files);
  auto n_files = std::make_unique<StringViews[]>(next_cap);
  std::memmove(n_files.get(), files.get(), sizeof(StringViews) * cap_files);
  auto n_hashes = std::make_unique<size_t[]>(next_cap);
  std::memmove(n_hashes.get(), hashes.get(), sizeof(size_t) * cap_files);

  auto n_deps = std::make_unique<vector<unsigned int>[]>(next_cap);
  for (auto i = size_t{}; i < cap_files; ++i) {
    n_deps[i] = std::move(deps[i]);
  }

  types = std::move(n_types);
  files = std::move(n_files);
  hashes = std::move(n_hashes);
  deps = std::move(n_deps);

  cap_files = next_cap;
}

auto Module::DepTree::reserve(size_t min) noexcept(false) -> void {
  if (cap_files > min) {
    return;
  }

  // these are basic types so we *should* just be able to memmov them
  auto n_types = std::make_unique<SourceFile_t[]>(min);
  auto n_files = std::make_unique<StringViews[]>(min);
  auto n_hashes = std::make_unique<size_t[]>(min);
  auto n_deps = std::make_unique<vector<unsigned int>[]>(min);

  std::memmove(n_types.get(), types.get(), sizeof(SourceFile_t) * cap_files);
  std::memmove(n_files.get(), files.get(), sizeof(StringViews) * cap_files);
  std::memmove(n_hashes.get(), hashes.get(), sizeof(size_t) * cap_files);
  for (auto i = size_t{}; i < cap_files; ++i) {
    n_deps[i] = std::move(deps[i]);
  }

  types = std::move(n_types);
  files = std::move(n_files);
  hashes = std::move(n_hashes);
  deps = std::move(n_deps);

  cap_files = min;
}

#ifdef DEBUG
auto Module::DepTree::display(std::ostream &out,
                              unsigned int const depth) const noexcept -> void {
  out << "All string = [" << string_view{all_paths.buffer, all_paths.size}
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
  switch (types[idx]) {
  case SourceFile_t::IMPL:
    out << "IMPL";
    break;
  case SourceFile_t::HEADER:
    out << "HEADER";
    break;
  case SourceFile_t::SYSTEM:
    out << "SYSTEM";
    break;
  case SourceFile_t::MISC:
    out << "MISC";
    break;
  };
  out << "\",\n";

  out << indents << "\"path\":\""
      << string_view{all_paths.buffer + files[idx].start,
                     all_paths.buffer + files[idx].end}
      << "\",\n";

  out << std::hex << indents << "\"hash\":" << hashes[idx] << ",\n";

  out << indents << "\"deps\":[\n";

  for (auto const dep_idx : deps[idx]) {
    display_impl(out, depth + 1, dep_idx);
  }

  out << indents << "]\n";

  out << indents << "}\n";
}
#endif // DEBUG

// TODO: idk make this better (faster, or smaller file size)
// TODO: update this function to return some error code or whatever
// TODO: update this to also include the files that were actually compiled for
// some kind of incrimental build process
auto Module::serialize(fs::path const &path) const -> void {
  auto outfile = File(path, File::WRITE | File::CREATE);
  if (!outfile) {
    return;
  }

  struct memory_buffer final {
    auto resize(size_t at_least = 0) noexcept -> void {
      auto const new_size = 3 * size / 2 + at_least;
      auto new_buffer = std::make_unique<u8[]>(new_size);
      memcpy(new_buffer.get(), buffer.get(), size);
      buffer = std::move(new_buffer);
      size = new_size;
    }
    auto write(void const *src, size_t n_bytes) noexcept -> void {
      if (cur + n_bytes >= size)
        resize(n_bytes);
      memcpy(buffer.get() + cur, src, n_bytes);
      cur += n_bytes;
    }
    auto write_string(std::string_view const str) noexcept -> void {
      auto const str_size = str.size();
      write(&str_size, sizeof(str_size));
      write(str.data(), str_size * sizeof(char));
    }
    auto write_vector(std::span<fs::path const> const span) noexcept -> void {
      auto const span_size = span.size();
      write(&span_size, sizeof(span_size));

      for (auto i = size_t{}; i < span_size; ++i) {
        write_string(span[i].string());
      }
    }

    std::unique_ptr<u8[]> buffer;
    size_t cur;
    size_t size;
  } mem = {std::make_unique<u8[]>(1024), 0, 1024};

  mem.write(&type, sizeof(type));

#pragma region DepTree
  mem.write(&tree.all_paths.size, sizeof(tree.all_paths.size));
  mem.write(tree.all_paths.buffer, tree.all_paths.size * sizeof(char));

  mem.write(&tree.num_files, sizeof(tree.num_files));

  // it would probably be better to have a vec_write function or something like
  // that, which would help with buffering, but this should be a fine hack for
  // now
  mem.write(tree.types.get(), sizeof(decltype(tree.types[0])) * tree.num_files);
  mem.write(tree.files.get(), sizeof(decltype(tree.files[0])) * tree.num_files);

  auto constexpr tree_dep_size = sizeof(decltype(tree.deps[0][0]));
  for (auto i = size_t{}; i < tree.num_files; ++i) {
    auto const dep_size = tree.deps[i].size();
    mem.write(&dep_size, sizeof(dep_size));

    mem.write(tree.deps[i].data(), dep_size * tree_dep_size);
  }

  mem.write(tree.hashes.get(),
            sizeof(decltype(tree.hashes[0])) * tree.num_files);

#pragma endregion DepTree

  mem.write_vector(roots);
  mem.write_vector(headers);
  mem.write_vector(includes);
  mem.write_vector(dep_includes);
  mem.write_vector(sys_includes);
  mem.write_vector(linking);

  // TODO: serialize pp::Interpreter
  mem.write_string(compiler);
  mem.write_string(name);
  mem.write_string(install_dir);

  // TODO: this part can fail and we should report it :)
  outfile.write(mem.buffer.get(), mem.cur, 1);
  outfile.flush();
#ifdef DEBUG
  std::cout << "serialized file [" << path.string() << "] with [" << mem.cur
            << "] byte\n";
#endif // DEBUG
}

// TODO: write this, figure out how to work with the pp::Interpreter macros
auto Module::deserialize(fs::path const &path)
    -> std::variant<Module, std::string> {
  auto file = File(path, File::READ | File::BINARY);
  if (!file) {
    return std::format("unable to open serialization file [{}]", path.c_str());
  }
  std::cout << "deserializing file [" << path.string() << "]\n";
  struct Deserializer final {
    constexpr Deserializer(File &file) noexcept
        : buf(nullptr), cur(0), size(0) {
      auto &&[fsize, fcontent] = file.dump_content();
      size = fsize;
      buf = std::move(fcontent);
    }

    auto check(size_t const amount, std::string_view const name) -> void {
      // i think this should be >=, but when that happens we seem to get false
      // positives when cur + amount == size(?)
      // i'm not smart enough with serialization to know if that is what's
      // supposed to happen or not
      if (cur + amount > size) {
        throw std::runtime_error(std::format(
            "Attempting to read [{}] bytes for [{}], but not enough bytes "
            "available in buffer. cur = [{}], size = [{}].",
            amount, name, cur, size));
      }
    }

#define read_basic_t(t)                                                        \
  auto read_##t() -> t {                                                       \
    check(sizeof(t), #t);                                                      \
    auto const res = static_cast<t>(buf[cur]);                                 \
    cur += sizeof(t);                                                          \
    return res;                                                                \
  }
    read_basic_t(size_t);
    read_basic_t(len_t);
    read_basic_t(Module_t);
#undef read_basic_t
    auto read_uint() -> unsigned int {
      check(sizeof(unsigned), "unsigned int");
      auto const res = static_cast<unsigned int>(buf[cur]);
      cur += sizeof(unsigned int);
      return res;
    }

    auto read_OwnedString() -> OwnedString {
      auto const str_size = read_len_t();
      if (cur + str_size >= size)
        unreachable();
      // we would normally have to allocate str_size + 1, but the null
      // terminator is being included in the cache (which is not intended and
      // when that's fixed this will need to be updated as well)
      auto str = (char *)malloc(sizeof(char) * str_size);
      auto res = OwnedString(str, str_size);
      memcpy(res.buffer, buf.get() + cur, sizeof(char) * str_size);
      res.size = str_size; // this shouldn't really be allowed, but i fucked
                           // up the api for OwnedString, and this seems
                           // like the only way to make this work :)
      cur += sizeof(char) * str_size;
      return res;
    }

    auto vec_read_types(size_t const vec_size)
        -> std::unique_ptr<DepTree::SourceFile_t[]> {
      check(vec_size * sizeof(DepTree::SourceFile_t), "SourceFile_t");

      auto res = std::make_unique<DepTree::SourceFile_t[]>(vec_size);
      memcpy(res.get(), buf.get() + cur,
             vec_size * sizeof(DepTree::SourceFile_t));
      cur += vec_size * sizeof(DepTree::SourceFile_t);
      return res;
    }
    auto vec_read_files(size_t const vec_size)
        -> std::unique_ptr<StringViews[]> {
      check(vec_size * sizeof(StringViews), "StringViews");

      auto res = std::make_unique<StringViews[]>(vec_size);
      memcpy(res.get(), buf.get() + cur, vec_size * sizeof(StringViews));
      cur += vec_size * sizeof(StringViews);
      return res;
    }
    auto vec_read_deps(size_t const vec_size)
        -> std::unique_ptr<vector<unsigned int>[]> {
      auto res = std::make_unique<vector<unsigned int>[]>(vec_size);
      for (auto i = size_t{}; i < vec_size; ++i) {
        auto const this_vec_size = read_size_t();
        // TODO: optimize this
        res[i].reserve(this_vec_size);
        for (auto j = size_t{}; j < this_vec_size; ++j) {
          res[i].push_back(read_uint());
        }
      }
      return res;
    }

    auto vec_read_hashes(size_t const vec_size) -> std::unique_ptr<size_t[]> {
      check(vec_size * sizeof(size_t), "Hashes");

      auto res = std::make_unique<size_t[]>(vec_size);
      memcpy(res.get(), buf.get() + cur, vec_size * sizeof(size_t));
      cur += vec_size * sizeof(size_t);
      return res;
    }

    auto read_std_string() -> std::string {
      auto res = std::string();
      auto const str_size = read_size_t();
      check(str_size * sizeof(char), "std::string");
      res.reserve(str_size);
      res.assign(buf.get() + cur, buf.get() + cur + str_size * sizeof(char));
      cur += str_size * sizeof(char);
      return res;
    }

    auto read_std_vector_fs_path() -> std::vector<fs::path> {
      auto res = vector<fs::path>();
      auto const vec_size = read_size_t();
      res.reserve(vec_size);
      for (auto i = size_t{}; i < vec_size; ++i) {
        res.emplace_back(fs::path(read_std_string()));
      }
      return res;
    }

    std::unique_ptr<u8[]> buf;
    size_t cur;
    size_t size;
  } mem = Deserializer(file);
  try {
    auto mod = Module();

    mod.type = mem.read_Module_t();

#pragma region DepTree
    // NOTE: i'm not sure if this is worth it, but if you initialize mod.tree =
    // DepTree(number), then it causes a memory leak, not fully sure where, but
    // we can just ignore it by not initializing it with anything
    mod.tree = DepTree();
    mod.tree.all_paths = mem.read_OwnedString();
    mod.tree.num_files = mem.read_size_t();
    mod.tree.types = mem.vec_read_types(mod.tree.num_files);
    mod.tree.files = mem.vec_read_files(mod.tree.num_files);
    // TODO: do the vector read of the vectors for the deps
    mod.tree.deps = mem.vec_read_deps(mod.tree.num_files);
    mod.tree.hashes = mem.vec_read_hashes(mod.tree.num_files);
#pragma endregion DepTree
    mod.roots = mem.read_std_vector_fs_path();
    mod.headers = mem.read_std_vector_fs_path();
    mod.includes = mem.read_std_vector_fs_path();
    mod.dep_includes = mem.read_std_vector_fs_path();
    mod.sys_includes = mem.read_std_vector_fs_path();
    mod.linking = mem.read_std_vector_fs_path();

    // TODO: deserialize pp::Interpreter

    mod.compiler = mem.read_std_string();
    mod.name = mem.read_std_string();
    mod.install_dir = mem.read_std_string();

    return mod;
  } catch (std::exception const &e) {
    return std::string(e.what());
  }
}

// TODO: optimize this, reorder equality checks, maybe in memory serialize the
// objects and just compare the bytes(?)
auto Module::operator==(Module const &that) const noexcept -> bool {
  if (type != that.type) {
    return false;
  }

  // tree
  if (tree.num_files != that.tree.num_files) {
    return false;
  }

  for (auto i = size_t{}; i < tree.num_files; ++i) {
    if (tree.hashes[i] != that.tree.hashes[i]) {
      return false;
    }
  }
  if (tree.all_paths.size != that.tree.all_paths.size) {
    return false;
  }
  if (strncmp(tree.all_paths.buffer, that.tree.all_paths.buffer,
              tree.all_paths.size)) {
    return false;
  }

  for (auto i = size_t{}; i < tree.num_files; ++i) {
    if (tree.types[i] != that.tree.types[i]) {
      return false;
    }
  }
  for (auto i = size_t{}; i < tree.num_files; ++i) {
    if (tree.files[i].start != that.tree.files[i].start &&
        tree.files[i].end != that.tree.files[i].end) {
      return false;
    }
  }
  for (auto i = size_t{}; i < tree.num_files; ++i) {
    if (tree.deps[i].size() != that.tree.deps[i].size()) {
      return false;
    }

    for (auto j = size_t{}; j < tree.deps[i].size(); ++j) {
      if (tree.deps[i][j] != that.tree.deps[i][j]) {
        return false;
      }
    }
  }

  // TODO: compare interpreters, i.e. the macros

  // rest of the class
  if (compiler != that.compiler) {
    return false;
  }

  if (name != that.name) {
    return false;
  }

  if (install_dir != that.install_dir) {
    return false;
  }

  return true;
}

// this function could probably have better error handling, but this is fine for
// now
auto Module::DepTree::get_file_content(FILE *file) noexcept(false)
    -> FixedString {
  if (fseek(file, 0, SEEK_END) == -1)
    throw CAPI(strerror(errno));

  auto const _fsize = ftell(file);
  if (_fsize == -1)
    throw CAPI(strerror(errno));

  auto fsize = static_cast<size_t>(_fsize);
  rewind(file);

  auto *fcontent = (char *)malloc(sizeof(char) * fsize + 1);
  if (fcontent == nullptr)
    throw CAPI(strerror(errno));

  if (auto const amount_read = fread(fcontent, sizeof(char), fsize, file);
      amount_read != fsize) {
    free(fcontent);
    throw CAPI(strerror(errno));
  }
  fcontent[fsize] = 0;
  return FixedString(fcontent, fsize);
}

#ifdef DEBUG
auto Module::display(std::ostream &out) const noexcept -> void {
  auto _display = [&](auto x) { out << x << ", "; };

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

  out << "headers = [";
  std::for_each(headers.begin(), headers.end(), _display);
  out << "]\n";

  out << "includes= [";
  std::for_each(includes.begin(), includes.end(), _display);
  out << "]\n";

  out << "dep_includes= [";
  std::for_each(dep_includes.begin(), dep_includes.end(), _display);
  out << "]\n";

  out << "sys_includes= [";
  std::for_each(sys_includes.begin(), sys_includes.end(), _display);
  out << "]\n";

  out << "linking = [";
  std::for_each(linking.begin(), linking.end(), _display);
  out << "]\n";

  out << "compiler = " << compiler << '\n';
  out << "name = " << name << '\n';
  out << "install_dir = " << install_dir << '\n';
  out.flush();
}
#endif // DEBUG

static auto include_path_cache =
    std::unordered_map<std::string_view, vector<fs::path>>();
// TODO: i think i'm not properly handling child procs, so see about fixing it
// in these two functions :)
// from some basic perf testing, these two functions seem to be the biggest slow
// downs, they should be run in parallel (or just in the background) which will
// help speed things up. We could rework the thread pool to allow for arbitrary
// functions to be run(?)
auto Module::append_include_paths(string_view const compiler) -> void {
  // TODO: see if we need to cache the whole compiler string, or if it's enough
  // to just cache the path to the binary, i.e. if we can get away with just
  // caching /usr/bin/clang, then we can use the cache more, and don't have to
  // actually go into the function that much
  if (auto includes = include_path_cache.find(compiler);
      includes != include_path_cache.end()) {
    sys_includes = includes->second;
    return;
  }
  auto _pipes = array<int, 2>{};
  if (pipe(_pipes.data()) == -1) {
    throw CAPI(strerror(errno));
  }
  auto &&[read_pipe, write_pipe] = _pipes;
  auto const pid = fork();
  if (pid < 0) {
    close(read_pipe);
    close(write_pipe);
    throw CAPI(strerror(errno));
  }
  switch (pid) {
  case 0: { // in child proc
    // close reader
    close(read_pipe);
    auto const command_string =
        std::format("{} -v -c -xc++ /dev/null -o {}/luamake_null.o",
                    /*string_view{compiler.data(), compiler_command_end},*/
                    compiler, fs::temp_directory_path().c_str());

    dup2(write_pipe, STDERR_FILENO);
    if (execl("/bin/sh", "sh", "-c", command_string.c_str(), nullptr) == -1) {
      close(write_pipe); // we never actually close the write_pipe, but execl
                         // replaces the running program so idk
      throw CAPI(strerror(errno));
    }
  } break;
  default: { // in parent proc
    // there's probably a better way of doing this, but this is the most
    // straightforward way i can think of
    close(write_pipe);

    auto includes = vector<fs::path>();
    includes.reserve(10);
    auto constexpr buffer_size = sizeof(char) * size_t{2 << 8};
    // code is technically unsafe, bc many of the std::string functions can
    // throw, leaking this buffer :)
    auto *const buffer = (char *)malloc(buffer_size + 1);
    if (buffer == nullptr)
      throw CAPI(strerror(errno));
    memset(buffer, 0, buffer_size + 1);

    auto search_string = string();
    search_string.reserve(256);
    auto amount_read = read(read_pipe, buffer, buffer_size);
    while (amount_read > 0) {
      search_string.append(buffer, static_cast<size_t>(amount_read));
      amount_read = read(read_pipe, buffer, buffer_size);
    }

    free(buffer);

    if (amount_read < 0) {
      auto const err = errno;
      // this can change errno, so to get the error we're interested in we have
      // to do this :)
      // we might want to also check if this errors, but i'll leave that for
      // someone else to do
      close(read_pipe);
      throw CAPI(strerror(err));
    }
    close(read_pipe);

    // reached EOF

    auto const include_start = search_string.find("#include <");
    ASSERT_ERROR(include_start == search_string.npos);

    auto const *start_path = search_string.data() + include_start;
    while (*start_path != 0 && *start_path != '\n')
      ++start_path;

    ASSERT_ERROR(*start_path == 0);

    start_path = skip_ws(start_path);
    for (auto end_path = start_path; string_view{start_path, end_path} !=
                                     string_view{"End of search list."};
         end_path = start_path) {
      while (*end_path != 0 && *end_path != '\n') {
        ++end_path;
      }
      if (*end_path == 0)
        break;
      // there's probably a better way to do this, but idk this is fine for now
      // :)
      if (fs::exists(fs::path(string_view(start_path, end_path)))) {
        includes.emplace_back(fs::canonical(fs::path(start_path, end_path)));
        /*
        sys_includes.emplace_back(
            fs::canonical(fs::path(start_path, end_path)));
            */
      }
      start_path = skip_ws(end_path);
    }
    include_path_cache[compiler] = includes;
    sys_includes = includes;
    (void)waitpid(pid, nullptr, WNOHANG);
  } break;
  }
}

// TODO: add another field for function macros, so that they can be added more
// easily, and (if needed) evaluated easier
static auto predefined_macros_cache =
    std::unordered_map<std::string_view,
                       std::pair<std::unordered_map<std::string, pp::Macro>,
                                 std::unordered_set<std::string>>>();
auto Module::append_predefined_macros(string_view const compiler)
    -> std::pair<std::unordered_map<std::string, pp::Macro>,
                 std::unordered_set<std::string>> {
  if (auto macros = predefined_macros_cache.find(compiler);
      macros != predefined_macros_cache.end()) {
    return macros->second;
  }
  auto _pipes = array<int, 2>{};
  if (pipe(_pipes.data()) == -1) {
    throw CAPI(strerror(errno));
  }
  auto &&[read_pipe, write_pipe] = _pipes;
  auto const pid = fork();
  if (pid < 0) {
    close(read_pipe);
    close(write_pipe);
    throw CAPI(strerror(errno));
  }
  switch (pid) {
  case 0: { // in child proc
    // close reader
    close(read_pipe);
    auto const command_string = std::format(
        "echo | {} -dM -E -", string_view{compiler.data(), compiler.find(' ')});

    dup2(write_pipe, STDOUT_FILENO);
    if (execl("/bin/sh", "sh", "-c", command_string.c_str(), nullptr) == -1) {
      close(write_pipe); // we never actually close the write_pipe, but execl
                         // replaces the running program so idk
      throw CAPI(strerror(errno));
    }
  } break;
  default: { // in parent proc
    auto macros = std::unordered_map<std::string, pp::Macro>();
    auto def_macros = std::unordered_set<std::string>();

    close(write_pipe);
    auto *read_me =
        fdopen(dup(read_pipe), "r"); // idk saw something on stackoverflow
    auto buffer_size = sizeof(char) * size_t{2 << 8};
    auto *buffer = (char *)malloc(buffer_size + 1);
    if (buffer == nullptr)
      throw CAPI(strerror(errno));
    memset(buffer, 0, buffer_size + 1);

    // this seems to work, bc getline returns -1 on EOF so we can't check
    // amount_read to see if there's an error or if we just hit EOF
    errno = 0;

    auto amount_read = getline(&buffer, &buffer_size, read_me);
    for (; amount_read > 0;
         amount_read = getline(&buffer, &buffer_size, read_me)) {
      auto constexpr header = string_view{"#define "};
      if (strncmp(buffer, header.data(), header.size()) != 0) {
        fclose(read_me);
        free(buffer);
        close(read_pipe);
        throw MisformattedOutput(
            std::format("echo | {} -dM -E -",
                        string_view{compiler.data(), compiler.find(' ')}));
      }

      auto macro_start = header.size();
      auto macro_cur = macro_start;
      while (macro_cur < buffer_size && buffer[macro_cur] != 0) {
        if (buffer[macro_cur] == ' ' || buffer[macro_cur] == '\n') {
          break;
        }
        ++macro_cur;
      }

      // TODO: add checks to make sure we're not out of bounds, but this seems
      // to be working fine now as a hack :)
      if (buffer[macro_cur] == ' ' && buffer[macro_cur + 1] == '\n') {
        def_macros.emplace(buffer + macro_start, buffer + macro_cur - 1);
        continue;
      }

      auto const macro_name =
          string_view{buffer + macro_start, buffer + macro_cur};

      if (buffer[macro_cur] != ' ') {
        fclose(read_me);
        free(buffer);
        close(read_pipe);
        throw UnexpectedCharacter(
            "Defined macro ended with unexpected character, expected ' '",
            buffer[macro_cur]);
      }
      macro_start = macro_cur + 1;
      ++macro_cur;
      // TODO: find a way to actually get the macros type (int, double,
      // string_literal, etc), really seems annoying bc we could also have
      // macros defined as like functions :)
      while (macro_cur < buffer_size && buffer[macro_cur] != 0) {
        if (buffer[macro_cur] == '\n')
          break;
        ++macro_cur;
      }

      macros.emplace(macro_name, pp::Macro{string(buffer + macro_start,
                                                  buffer + macro_cur)});
    }

    if (errno != 0) {
      throw CAPI(strerror(errno));
    }

    (void)waitpid(pid, nullptr, WNOHANG);
    fclose(read_me);
    free(buffer);
    close(read_pipe);

    // reached EOF
    auto res = std::make_pair(std::move(macros), std::move(def_macros));
    predefined_macros_cache[compiler] = res;
    return res;
  } break;
  }
  unreachable();
}

// TODO: change this to just keep track of the index for the table, we don't
// need to worry about popping the values from the stack necessarily as this
// function is (mostly) called from lua itself and so we have a new stack to
// work with, see [[https://www.lua.org/manual/5.4/manual.html]] section 4 for
// more info
Module::Module(Module_t &&type, lua_State *state, fs::path const &root)
    : type(type), tree(8), roots(), headers(), includes(), sys_includes(),
      linking(), interpreter({}, {}), compiler(), name(), install_dir() {
  switch (auto const name_t = lua_getfield(state, -1, "name")) {
  case LUA_TSTRING:
    name = lua_tolstring(state, -1, nullptr);
    break;
  case LUA_TNIL:
    throw MissingField("name");
  default:
    throw UnexpectedType("name", LUA_TSTRING, name_t);
  }
  lua_pop(state, 1);

  switch (type) {
  case EXE:
    roots.reserve(1);
    switch (auto const root_t = lua_getfield(state, -1, "root")) {
    case LUA_TSTRING:
      roots.push_back(
          fs::canonical(root / fs::path(lua_tolstring(state, -1, nullptr))));
      break;
    case LUA_TNIL:
      throw MissingField("root");
    default:
      throw UnexpectedType("root", LUA_TSTRING, root_t);
    }
    lua_pop(state, 1);
    break;
  case STATIC: {
    switch (auto const root_t = lua_getfield(state, -1, "roots")) {
    case LUA_TTABLE: {
      auto const num_roots = lua_rawlen(state, -1);
      auto roots_tbl = -1;
      for (auto i = 1; i <= num_roots; ++i) {
        auto const value_t = lua_geti(state, roots_tbl, i);
        if (value_t != LUA_TSTRING) {
          throw UnexpectedType("roots[i]", LUA_TSTRING, value_t);
        }
        roots.push_back(
            fs::canonical(root / fs::path(lua_tolstring(state, -1, nullptr))));
        --roots_tbl;
      }
      lua_pop(state, static_cast<int>(num_roots) + 1);
    } break;
    case LUA_TNIL:
      throw MissingField("roots");
    default:
      throw UnexpectedType("roots", LUA_TTABLE, root_t);
    }
  } break;
  case DYNAMIC:
    std::cerr << "Not currently implimented\n";
    std::terminate();
    break;
  }

  switch (auto const compiler_t = lua_getfield(state, -1, "compiler")) {
  case LUA_TTABLE:
    compiler = Module::parse_compiler_table(state);
    break;
  case LUA_TNIL:
    throw MissingField("compiler");
  default:
    throw UnexpectedType("compiler", LUA_TTABLE, compiler_t);
  }
  lua_pop(state, 1);

  auto res = std::async(std::launch::async, [this]() {
    this->append_include_paths(this->compiler);
  });
  auto macros_res = std::async(std::launch::async, [this]() {
    return this->append_predefined_macros(this->compiler);
  });
  // auto &&[macros, def_macros] = append_predefined_macros(compiler);

  switch (auto const install_dir_t = lua_getfield(state, -1, "install_dir")) {
  case LUA_TSTRING:
    install_dir = (root / lua_tolstring(state, -1, nullptr)).string();
    break;
  case LUA_TNIL:
    throw MissingField("install_dir");
  default:
    throw UnexpectedType("install_dir", LUA_TSTRING, install_dir_t);
  }
  lua_pop(state, 1);

  switch (type) {
  case Module_t::EXE: {
    includes.reserve(1);
    auto const test = fs::canonical(roots[0]).parent_path();
    includes.push_back(test);
  } break;
  case Module_t::STATIC:
    includes.reserve(roots.size());
    for (auto const &root : roots) {
      auto const found = std::find(includes.begin(), includes.end(),
                                   fs::canonical(root.parent_path()));
      if (found != includes.end()) {
        includes.push_back(fs::canonical(root.parent_path()));
      }
    }
  case Module_t::DYNAMIC:
    break;
  }

  switch (auto const include_t = lua_getfield(state, -1, "include")) {
  case LUA_TTABLE: {
    auto const len = lua_rawlen(state, -1);
    auto include = -1;
    for (auto i = 1; i <= len; ++i) {
      switch (auto const value_t = lua_geti(state, include, i)) {
      case LUA_TSTRING:
        includes.push_back(lua_tolstring(state, -1, nullptr));
        break;
      default:
        throw UnexpectedType("include[i]", LUA_TSTRING, value_t);
      }
      --include;
    }
    lua_pop(state, static_cast<int>(len));
  } break;
  case LUA_TNIL:
    break;
  default:
    throw UnexpectedType("include", LUA_TTABLE, include_t);
  }
  lua_pop(state, 1);

  // TODO: rework this, only use it for system/library includes that we
  // (luamake) doesn't control
  switch (auto const linking_t = lua_getfield(state, -1, "linking")) {
  case LUA_TTABLE: {
    auto const len = lua_rawlen(state, -1);
    linking.reserve(len);
    auto linking_idx = -1;
    for (auto i = 1; i <= len; ++i) {
      switch (auto const value_t = lua_geti(state, linking_idx, i)) {
      case LUA_TSTRING:
        linking.push_back(lua_tolstring(state, -1, nullptr));
        break;
      default:
        throw UnexpectedType("linking[i]", LUA_TSTRING, value_t);
      }
      --linking_idx;
    }
    lua_pop(state, static_cast<int>(len));
  } break;
  case LUA_TNIL:
    break;
  default:
    throw UnexpectedType("linking", LUA_TTABLE, linking_t);
  }
  lua_pop(state, 1);

  switch (type) {
  case Module_t::EXE:
    break;
  // this needs to not be nullable
  case Module_t::STATIC: {
    switch (auto const header_t = lua_getfield(state, -1, "headers")) {
    case LUA_TTABLE: {
      auto const len = lua_rawlen(state, -1);
      auto headers_tbl = -1;
      for (auto i = 1; i <= len; ++i) {
        switch (auto const value_t = lua_geti(state, headers_tbl, i)) {
        case LUA_TSTRING: {
          headers.push_back(root / fs::path(lua_tostring(state, -1)));
        } break;
        default:
          throw UnexpectedType("headers[i]", LUA_TSTRING, value_t);
        }
        --headers_tbl;
      }
      lua_pop(state, static_cast<int>(len));
    } break;
    case LUA_TNIL:
      break;
    default:
      throw UnexpectedType("headers", LUA_TTABLE, header_t);
    }
    lua_pop(state, 1);
  } break;
  case Module_t::DYNAMIC:
    break;
  }

  auto &&[macros, def_macros] = macros_res.get();
  switch (auto const macro_t = lua_getfield(state, -1, "macros")) {
  case LUA_TTABLE: {
    auto const len = lua_rawlen(state, -1);
    auto macros = -1;
    for (auto i = 1; i <= len; ++i) {
      auto const value_t = lua_geti(state, macros, i);
      if (value_t != LUA_TSTRING) {
        throw UnexpectedType("macros[i]", LUA_TSTRING, value_t);
      }
      auto mac = string(lua_tolstring(state, -1, nullptr));
      if (mac.find('=') != mac.npos) {
        // TODO: parse macro being set to value
      } else {
        def_macros.insert(std::move(mac));
      }
      --macros;
    }
    lua_pop(state, static_cast<int>(len) + 1);
  } break;
  case LUA_TNIL:
    break;
  default:
    throw UnexpectedType("macros", LUA_TTABLE, macro_t);
  }

  interpreter = pp::Interpreter(std::move(macros), std::move(def_macros));
  res.get();
}

auto Module::gen_dep_tree() -> void {
  // we probably don't need this, b/c the constructor will be called when we
  // originally construct this module
  // [[tree = DepTree();]]
  for (auto const &root : roots) {
    append_dep(root, DepTree::ROOT_IDX);
  }
}

auto Module::format_includes() const -> std::string {
  auto include_func = [](auto &&e, auto &&next) {
    return std::format("{} -I{}", e, next.string());
  };
  auto dep_func = [](auto &&e, auto &&next) {
    return std::format("{} -iquote {}", e, next.parent_path().string());
  };
  auto sys_func = [](auto &&e, auto &&next) {
    return std::format("{} -isystem {}", e, next.string());
  };
  return std::accumulate(includes.cbegin(), includes.cend(), std::string(),
                         include_func) +
         std::accumulate(dep_includes.cbegin(), dep_includes.cend(),
                         std::string(), dep_func) +
         std::accumulate(sys_includes.cbegin(), sys_includes.cend(),
                         std::string(), sys_func);
}

auto Module::format_links() const -> std::string {
  // when we add dynamic library support, we'll have to worry about the -L
  // flag and shit
  auto res = string();
  res.reserve(256); // idk random number can def be optimized :)
  for (auto const &path : linking) {
    res += std::format("{} ", path.string());
  }
  return res;
}

// TODO: double check that it's actually fine to throw an exception here and
// that this won't cause a memory leak, ig it's fine if it does cause a memory
// leak because this errors out to the top, but it's still a concern
auto Module::parse_compiler_table(lua_State *state) -> string {
  auto str = string();

  auto const compiler_idx = lua_absindex(state, -1);

  auto const compiler_t = lua_getfield(state, compiler_idx, "compiler");
  switch (compiler_t) {
  case LUA_TSTRING:
    str += lua_tolstring(state, -1, nullptr);
    break;
  case LUA_TNIL:
    throw MissingField("compiler.compiler");
  default:
    throw UnexpectedType("compiler.compiler", LUA_TSTRING, compiler_t);
  }

  // TODO: honestly probably write a macro to make parsing optional and required
  // table entries easier
  auto const optimize_t = lua_getfield(state, compiler_idx, "optimize");
  switch (optimize_t) {
  case LUA_TSTRING:
    str += " -";
    str += lua_tolstring(state, -1, nullptr);
    break;
  case LUA_TNIL:
    break;
  default:
    throw UnexpectedType("compiler.optimize", LUA_TSTRING, optimize_t);
  }

  auto const warnings_t = lua_getfield(state, compiler_idx, "warnings");
  switch (warnings_t) {
  case LUA_TTABLE: {
    auto const warnings_idx = lua_absindex(state, -1);

    lua_pushnil(state);
    while (lua_next(state, warnings_idx) != 0) {
      if (lua_type(state, -1) != LUA_TSTRING) {
        lua_pushstring(
            state,
            "Incorrect type in `warnings` table"); // TODO: update this to
                                                   // include the found type
        lua_error(state);
        return "";
      }
      str += " -";
      str += lua_tolstring(state, -1, nullptr);
      lua_pop(state, 1);
    }
  } break;
  case LUA_TNIL:
    break;
  default:
    throw UnexpectedType("compiler.warnings", LUA_TTABLE, warnings_t);
  }

  // *should* always exist, we'll just assume it exists for now
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

auto Builder::new_exe(lua_State *state) noexcept -> int {
  LUA_EXPECTED_ARGUMENTS(state, 2, new_exe);
  LUA_ASSERT_FORMAT(state, ret_t, lua_type(state, -1), LUA_TTABLE,
                    "Expected type of argument to `new_exe` to be of type "
                    "table, found [%s]",
                    lua_typename(ret_t));
  LUA_ASSERT_FORMAT(state, ret_t, lua_type(state, -2), LUA_TTABLE,
                    "Expected type of argument to `new_exe` to be of type "
                    "table, found [%s]",
                    lua_typename(ret_t));

  try {
    auto const root_t = lua_geti(state, -2, 1);
    if (root_t != LUA_TSTRING) {
      throw std::runtime_error(
          "idk what happened, but an internal error occured, where the type of "
          "a value was modified when it shouldn't have.");
    }
    auto const root = fs::path(lua_tolstring(state, -1, nullptr));
    lua_pop(state, 1); // need to pop the value off the stack now that we've
                       // taken over it, and so that the config obj is at the
                       // top of the stack for the Module function

    auto index_fut = std::async(std::launch::async, [&root]() -> lua_Integer {
      return static_cast<lua_Integer>(mods.contains(root));
    });

    auto exe_mod = builtins::Module(builtins::Module::EXE, state, root);
    exe_mod.gen_dep_tree();

    auto const idx = index_fut.get();
    if (idx == lua_Integer{-1}) {
      std::cerr << "Module " << root.string()
                << " does not exist in the lake modules currently known.\n";
      std::terminate();
    }

    mods.emplace_at(idx, std::move(exe_mod));

    lua_pushinteger(state, idx);

    return 1;
  } catch (ModuleErr const &e) {
    lua_pushstring(state, e.what().c_str());
    return lua_error(state);
  } catch (DepTreeErr const &e) {
    lua_pushstring(state, e.what().c_str());
    return lua_error(state);
  } catch (pp::Exception const &e) {
    lua_pushstring(state, e.what().c_str());
    return lua_error(state);
  } catch (std::exception const &e) {
    lua_pushstring(state, e.what());
    return lua_error(state);
  } catch (...) {
    lua_pushstring(state, "Unfortunately an error occured");
    return lua_error(state);
  }
  (void)lua_pushstring(state, "new_exe not impl");
  return lua_error(state);
}

auto Builder::new_static(lua_State *state) noexcept -> int {
  LUA_EXPECTED_ARGUMENTS(state, 2, new_static);
  LUA_ASSERT_FORMAT(state, ret_t, lua_type(state, -1), LUA_TTABLE,
                    "Expected type of argument to `new_static` to be of type "
                    "table, found [%s]",
                    lua_typename(ret_t));
  LUA_ASSERT_FORMAT(state, ret_t, lua_type(state, -2), LUA_TTABLE,
                    "Expected type of argument to `new_static` to be of type "
                    "table, found [%s]",
                    lua_typename(ret_t));

  try {
    auto const root_t = lua_geti(state, -2, 1);
    if (root_t != LUA_TSTRING) {
      throw std::runtime_error(
          "idk what happened, but an internal error occured, where the type of "
          "a value was modified when it shouldn't have.");
    }
    auto const root = fs::path(lua_tolstring(state, -1, nullptr));
    lua_pop(state, 1); // need to pop the value off the stack now that we've
                       // taken over it, and so that the config obj is at the
                       // top of the stack for the Module function

    auto index_fut = std::async(std::launch::async, [&root]() -> lua_Integer {
      return static_cast<lua_Integer>(mods.contains(root));
    });

    auto static_mod = builtins::Module(builtins::Module::STATIC, state, root);
    static_mod.gen_dep_tree();

    auto const idx = index_fut.get();
    if (idx == lua_Integer{-1}) {
      std::cerr << "Module " << root.string()
                << " does not exist in the lake modules currently known.";
      std::terminate();
    }

    mods.emplace_at(idx, std::move(static_mod));

    lua_pushinteger(state, idx);
    return 1;
  } catch (ModuleErr const &e) {
    lua_pushstring(state, e.what().c_str());
    return lua_error(state);
  } catch (DepTreeErr const &e) {
    lua_pushstring(state, e.what().c_str());
    return lua_error(state);
  } catch (pp::Exception const &e) {
    lua_pushstring(state, e.what().c_str());
    return lua_error(state);
  } catch (std::exception const &e) {
    lua_pushstring(state, e.what());
    return lua_error(state);
  } catch (...) {
    lua_pushstring(state, "Unfortunately an error occured");
    return lua_error(state);
  }
}

auto Builder::install_exe(lua_State *state) noexcept -> int {
  LUA_EXPECTED_ARGUMENTS(state, 1, install_exe)
  LUA_ASSERT_FORMAT(state, ret_t, lua_type(state, -1), LUA_TNUMBER,
                    "Expected type of argument to `install_exe` to be of type "
                    "integer, found [%s]",
                    lua_typename(ret_t));

  try {
    auto const mod_idx = lua_tointeger(state, -1);
    lua_pop(state, 1);
    auto const &parent_path = mods.get_module_path(mod_idx).parent_path();

    auto const &exe_mod = mods.module_at(mod_idx);
    if (exe_mod.type != builtins::Module::EXE) {
      throw std::runtime_error(std ::format(
          "module type is not exe, found [{}]",
          static_cast<std::underlying_type_t<builtins::Module::Module_t>>(
              exe_mod.type)));
    }

    // TODO: update these functions to throw exceptions
    // TODO: see if there is a performance increase by checking if the directory
    // is made and not making it if it is most of the time the directory will be
    // there, it's just annoying because we have to check every time in case
    // somebody changes the install_dir variable
    auto const install_dir =
        parent_path /
        fs::path(std::format("{}/{}.o", exe_mod.install_dir, exe_mod.name));
    auto ec = std::error_code{};
    if (fs::create_directories(install_dir, ec); ec) {
      std::cerr << ec.message() << '\n';
      lua_pushstring(state, "Unable to create directory");
      return lua_error(state);
    }

    if (fs::create_directories(
            fs::path(std::format("{}/__luamake_cache", exe_mod.install_dir)),
            ec);
        ec) {
      std::cerr << ec.message() << '\n';
      lua_pushstring(state, "Unable to create directory");
      return lua_error(state);
    }

    // rework this caching situation when the caching is actually working
    auto const cache_path = fs::path(std::format(
        "{}/__luamake_cache/{}.cache", exe_mod.install_dir, exe_mod.name));

    // NOTE: we might be able to put this on a background thread, then just
    // continue on doing things, and when this is done we do the comparison, but
    // for now we'll have this be blocking :)
    auto const maybe_cached_mod = builtins::Module::deserialize(cache_path);
    /* compare the current mod with the cached mod */
    switch (maybe_cached_mod.index()) {
    case 0: {
      auto const &cached_mod = std::get<builtins::Module>(maybe_cached_mod);
      if (exe_mod == cached_mod) {
        return 0;
      }
    } break;
    case 1: {
      auto const &error_message = std::get<std::string>(maybe_cached_mod);
      std::cerr << std::format("Error message = [{}]\n", error_message);
    } break;
    default:
      unreachable();
    }

    // this still means that we have to wait for these to finish before we can
    // continue on, i.e. we have to hang, the issue then can probably be solved
    // by moving these into static memory, along side the modules vector
    struct {
      std::mutex str_mtx;
      std::string actually_compiled_files;
    } res;
    threads.add_dep_tree_tasks(exe_mod.tree);

    // this takes a lot of parameters by ref, idk if that's something that we
    // should be doing
    // there *might* be some issues taking exe_mod, by ref, it will point to
    // something in static memory, but there might be some issues with it
    threads.add_task([&exe_mod, &res, &state, cache_path]() {
      auto const invoked_command = std::format(
          "{} -o {}/{} {} {}", exe_mod.compiler, exe_mod.install_dir,
          exe_mod.name, res.actually_compiled_files, exe_mod.format_links());
      std::cout << '[' << invoked_command << "]\n";
      std::cout.flush();
      // NOTE: figure out how to handle errors with the lua vm, if there's
      // internal mutex's that will stop conflicting and corrupting the stack,
      // or if we have to worry about a mutex around the lua vm ourselves
      // we might have to move some of the error handling into the global scope,
      // that way we can access it across threads and communicate with it
      // through the lua vm
      if (OS_CALL(invoked_command.c_str()) != 0) {
        lua_pushfstring(state, "Error compiling [%s]", invoked_command.c_str());
        return lua_error(state);
      } else {
        exe_mod.serialize(cache_path);
        return 0;
      }
    });
    return 0;
  } catch (ModuleErr const &e) {
    lua_pushstring(state, e.what().c_str());
    return lua_error(state);
  } catch (DepTreeErr const &e) {
    lua_pushstring(state, e.what().c_str());
    return lua_error(state);
  } catch (pp::Exception const &e) {
    lua_pushstring(state, e.what().c_str());
    return lua_error(state);
  } catch (std::exception const &e) {
    lua_pushstring(state, e.what());
    return lua_error(state);
  } catch (...) {
    lua_pushstring(state, "Unfortunately an error occured");
    return lua_error(state);
  }
}

auto Builder::install_static(lua_State *state) noexcept -> int {
  LUA_EXPECTED_ARGUMENTS(state, 1, install_static);
  LUA_ASSERT_FORMAT(state, ret_t, lua_type(state, -1), LUA_TNUMBER,
                    "Expected type of argument to `install_static` "
                    "to be of type integer, found [%s]",
                    lua_typename(ret_t));

  try {
#if 0
    auto const mod_idx = lua_tointeger(state, -1);
    lua_pop(state, 1);

    // i'm not sure if we actually need this variable now that we're using
    // everything as an absolute path but i'm not going to test that right now
    // and break everything :)
    auto const &parent_path = mods.get_module_path(mod_idx).parent_path();

    auto const &static_mod = mods.module_at(mod_idx);
    // TODO: better error handling with this
    if (static_mod.type != builtins::Module::STATIC) {
      throw std::runtime_error(std ::format(
          "module type is not static, found [{}]",
          static_cast<std::underlying_type_t<builtins::Module::Module_t>>(
              static_mod.type)));
    }

    auto const install_dir =
        parent_path / fs::path(std::format("{}/{}.o", static_mod.install_dir,
                                           static_mod.name));
    std::cout << std::format("making directory [{}]", install_dir.string())
              << '\n';
    std::cout.flush();
    auto ec = std::error_code{};
    if (fs::create_directories(install_dir, ec); ec) {
      lua_pushfstring(state, "Unable to create directory\n\t[%s]",
                      ec.message().c_str());
      return lua_error(state);
    }

    // TODO: try to move these calls to create directory to be do when the
    // initial project is set up, that way we don't have to worry about trying
    // to make them everytime which will slow things down on average
    if (fs::create_directories(fs::path(
            std::format("{}/__luamake_cache", static_mod.install_dir)));
        ec) {
      std::cerr << ec.message() << '\n';
      lua_pushstring(state, "Unable to create directory");
      return lua_error(state);
    }

    auto const cache_path =
        fs::path(std::format("{}/__luamake_cache/{}.cache",
                             static_mod.install_dir, static_mod.name));
    // NOTE: see note in install_exe
    auto const maybe_cached_mod = builtins::Module::deserialize(cache_path);
    switch (maybe_cached_mod.index()) {
    case 0: {
      auto const &cached_mod = std::get<builtins::Module>(maybe_cached_mod);
      if (static_mod == cached_mod) {
        return 0;
      }
    } break;
    case 1: {
      auto const &error_message = std::get<std::string>(maybe_cached_mod);
      std::cerr << std::format("Error message = [{}]\n", error_message);
    } break;
    default:
      unreachable();
    }

    auto const compiled_files = builtins::Compiler::compile(static_mod);
    auto const invoked_command =
        std::format("ar crs {}/lib{}.a {}", static_mod.install_dir,
                    static_mod.name, compiled_files);

    std::cout << '[' << invoked_command << "]\n";
    std::cout.flush();
    if (OS_CALL(invoked_command.c_str()) != 0) {
      lua_pushstring(
          state, std::format("Error compiling [{}]", invoked_command).c_str());
      return lua_error(state);
    }

    fs::create_directory(parent_path /
                         fs::path(std::format("{}/{}", static_mod.install_dir,
                                              static_mod.name)));

    auto const formatted_files =
        std::accumulate(static_mod.headers.begin(), static_mod.headers.end(),
                        std::string(), [](auto &&e, auto &&next) {
                          return std::format("{} {}", e, next.string());
                        });

    auto const copy_headers = std::format(
        "cp --target-directory={} {}",
        (parent_path / static_mod.install_dir / static_mod.name).string(),
        formatted_files);
    std::cout << '[' << copy_headers << "]\n";
    std::cout.flush();
    if (OS_CALL(copy_headers.c_str()) != 0) {
      lua_pushstring(
          state,
          std::format("Error moving headers [{}]", copy_headers).c_str());
      return lua_error(state);
    }

    // if nothing goes wrong we can serialize the whole data, check the note in
    // install_exe for more details about improvements
    static_mod.serialize(cache_path);
    return 0;
#endif
    lua_pushstring(
        state,
        "install_static function is not currently working :), will fix later");
    return lua_error(state);
  } catch (ModuleErr const &e) {
    lua_pushstring(state, e.what().c_str());
    return lua_error(state);
  } catch (DepTreeErr const &e) {
    lua_pushstring(state, e.what().c_str());
    return lua_error(state);
  } catch (pp::Exception const &e) {
    lua_pushstring(state, e.what().c_str());
    return lua_error(state);
  } catch (std::exception const &e) {
    lua_pushstring(state, e.what());
    return lua_error(state);
  } catch (...) {
    lua_pushstring(state, "Unfortunately an error occured");
    return lua_error(state);
  }
}

// NOTE: this function pushes a new builder object onto the stack, which means
// that if you want to test subprojects you can't really (because the
// LUAMAKE_TEST#n macro won't be included in the compilation)
auto Builder::build_dep(lua_State *state) noexcept -> int {
  LUA_EXPECTED_ARGUMENTS(state, 1, build_dep);
  LUA_ASSERT_FORMAT(state, ret_t, lua_type(state, -1), LUA_TNUMBER,
                    "Expected type of argument to function "
                    "`build_dep` to be number, found [%s]",
                    lua_typename(ret_t));

  try {
    auto const luamake_path = mods.get_module_path(lua_tointeger(state, -1));
    lua_pop(state, 1);

    if (luaL_dofile(state, luamake_path.c_str()) != LUA_OK) {
      (void)lua_pushfstring(
          state,
          "Unable to run the `luamake.lua` file required, in directory [%s]",
          luamake_path.parent_path().c_str());
      return lua_error(state);
    }

    LUA_ASSERT_FORMAT(state, ret_t, lua_type(state, -1), LUA_TTABLE,
                      "Expected table, found [%s]", lua_typename(ret_t));

    if (auto const build_func_t = lua_getfield(state, -1, "Build");
        build_func_t != LUA_TFUNCTION) {
      (void)lua_pushfstring(
          state,
          "Expected `Build` to have type function when "
          "returned in a table from the `luamake.lua` script at [%s]",
          luamake_path.c_str());
      return lua_error(state);
    }

    builtins::make_builder_obj(state);
    lua_pushstring(state, luamake_path.parent_path().c_str());
    lua_seti(state, -2, lua_Integer{1});
    if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
      return lua_error(state);
    }
    return 0;
  } catch (std::exception const &e) {
    lua_pushfstring(state, "%s", e.what());
    return lua_error(state);
  } catch (...) {
    lua_pushstring(state, "Unfortunately an error has occured");
    return lua_error(state);
  }

  lua_pushstring(
      state,
      std::format(
          "Unreachable point reached :), please report this. Function [{}]",
          __FUNCTION__)
          .c_str());
  return lua_error(state);
}

auto dump_impl(lua_State *state, unsigned int const depth) noexcept -> void {
  auto const indents = [](auto depth) { return std::string(depth, '\t'); };
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
      std::cout << indents(depth);
      switch (auto const key_t = lua_type(state, -2)) {
      case LUA_TNUMBER:
        std::cout << '[' << lua_tointeger(state, -2) << ']';
        break;
      case LUA_TSTRING:
        std::cout << '[' << lua_tolstring(state, -2, nullptr) << ']';
        break;
      default:
        std::cout << lua_typename(key_t);
        break;
      }

      std::cout << ':';
      dump_impl(state, depth + 1);

      lua_pop(state, 1);
    }
    std::cout << indents(depth - 1) << '}';
  } break;
  default:
    std::cout << lua_typename(type);
    break;
  }
  std::cout << '\n';
}

// TODO: switch this to use userdata, which should make things faster to process
// TODO: double check that this function isn't doing redundant type checks
// TODO: change this function to use the compiler_impl function
auto Builder::clang(lua_State *state) noexcept -> int {
  auto const num_args = lua_gettop(state);
  if (num_args != 1) {
    lua_pushstring(state, "Expected one argument to the clang function");
    return lua_error(state);
  }
  auto constexpr compiler_field = string_view{"clang"};
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
  auto compiler_path_fut =
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

  // TODO: allow for tables to be used so that instead of having to say
  // -I.,
  // -Isrc/external/...,
  // etc,
  // you could just do
  // I = {
  //  ".",
  //  "src/external/...",
  // }
  // And the -I will be added to the front of each
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
          "from using anything other than a number (i.e. passing an array), "
          "or "
          "a string for a key+value pair item, or a combination of the two");
      return lua_error(state);
    }
  }

  lua_setfield(state, ret_tbl_idx, "opt_args");

  // wait until the very end to let the async function run the longest, idk if
  // this is a good thing i'm bad with async stuff
  auto const path_to_compiler = compiler_path_fut.get();
  if (path_to_compiler == fs::path()) {
    lua_pushstring(state, "Unable to find clang binary.");
    return lua_error(state);
  }
  lua_pushstring(state, path_to_compiler.c_str());
  // lua_pushstring(state, compiler_field.data());
  lua_setfield(state, ret_tbl_idx, "compiler");

  return 1;
}

auto Builder::gcc_bare(lua_State *state) noexcept -> int {
  try {
    return compiler_impl(state, "gcc");
  } catch (std::exception const &e) {
    lua_pushstring(state, e.what());
    return lua_error(state);
  } catch (...) {
    lua_pushstring(state,
                   "An unknown exception was throw in the gcc_bare function");
    return lua_error(state);
  }
}

auto Builder::clang_bare(lua_State *state) noexcept -> int {
  try {
    return compiler_impl(state, "clang");
  } catch (std::exception const &e) {
    lua_pushstring(state, e.what());
    return lua_error(state);
  } catch (...) {
    lua_pushstring(state,
                   "An unknown exception was throw in the clang_bare function");
    return lua_error(state);
  }
}

auto Builder::require(lua_State *state) noexcept -> int {
  // because this is a function on an api boundary, we have to make sure that no
  // exceptions leak from it
  LUA_EXPECTED_ARGUMENTS(state, 2, require)
  LUA_ASSERT_FORMAT(state, arg_t, lua_type(state, -2), LUA_TTABLE,
                    "Expected table to require function, found [%s]",
                    lua_typename(arg_t));
  LUA_ASSERT_FORMAT(state, arg_t, lua_type(state, -1), LUA_TSTRING,
                    "Expected string to require function, found [%s]",
                    lua_typename(arg_t));
  try {
    auto fpath = [](lua_State *state) -> fs::path {
      auto const parent_path_t = lua_geti(state, -2, lua_Integer{1});
      switch (parent_path_t) {
      case LUA_TSTRING: {
      } break;
      case LUA_TNIL: {
        throw std::runtime_error(std::format(
            "Got nil for b[1], forgot to set the parent path variable"));
      } break;
      default:
        throw std::runtime_error(
            std::format("expected type of b[1] to be string, got [{}]",
                        lua_typename(parent_path_t)));
        break;
      }
      auto const parent_path = fs::path(lua_tolstring(state, -1, nullptr));
      auto const fname =
          parent_path /
          fs::path(string(lua_tolstring(state, -2, nullptr)) + ".lua");
      return fs::canonical(fname);
    }(state);
    // pop all arguments from the stack, and the b[1] that was pushed earlier
    lua_pop(state, 3);

    auto ec = std::error_code{};
    if ((void)fs::exists(fpath, ec); ec) {
      // TODO: better error handling :)
      // basically just copy what we do with the main.cpp run function
      lua_pushstring(state, ec.message().c_str());
      return lua_error(state);
    }

    (void)lua_pushinteger(state, mods.new_module(std::move(fpath)));
    return 1;
  } catch (std::exception const &e) {
    (void)lua_pushfstring(
        state, "An exception was encountered in the requires function, [%s]",
        e.what());
    return lua_error(state);
  } catch (...) {
    (void)lua_pushstring(
        state, "An unknown exception was encountered in the requires function");
    return lua_error(state);
  }
}

auto Builder::link_lib(lua_State *state) noexcept -> int {
  LUA_EXPECTED_ARGUMENTS(state, 2, require)
  LUA_ASSERT_FORMAT(state, arg_t, lua_type(state, -2), LUA_TNUMBER,
                    "Expected integer to `link_lib` function, found [%s]",
                    lua_typename(arg_t));
  LUA_ASSERT_FORMAT(state, arg_t, lua_type(state, -1), LUA_TNUMBER,
                    "Expected integer to `link_lib` function, found [%s]",
                    lua_typename(arg_t));
  try {
    auto const lib_to_be_linked = lua_tointeger(state, -2);
    auto const lib_getting_diddled = lua_tointeger(state, -1);

    auto const &mod_linked = mods.module_at(lib_to_be_linked);
    auto &mod_d = mods.module_at(lib_getting_diddled);

    // this should be correct, basically stolen from the install_static
    // function, there shouldn't be any issues, because the install_static
    // function just dumps all the headers in the same out directory
    mod_d.dep_includes.push_back(
        mods.get_module_path(lib_to_be_linked) /
        fs::path(
            std::format("{}/{}", mod_linked.install_dir, mod_linked.name)));
    mod_d.linking.push_back(fs::path(mod_linked.install_dir) /
                            ("lib" + mod_linked.name + ".a"));

    return 0;
  } catch (std::exception const &e) {
    (void)lua_pushfstring(
        state, "An exception was encountered in the requires function, [%s]",
        e.what());
    return lua_error(state);
  } catch (...) {
    (void)lua_pushstring(
        state, "An unknown exception was encountered in the requires function");
    return lua_error(state);
  }
}

auto Builder::get_os(lua_State *state) noexcept -> int {
  LUA_EXPECTED_ARGUMENTS(state, 0, get_os);
  try {
    // for now we're going to be assuming that the host machine you're on is the
    // one that you're building the libraries for, i do want to add a way to
    // enable cross compilation out of the box, but i'm not sure how to do that
    (void)lua_pushstring(state,
#if defined(_WIN32)
                         "windows"
#elif defined(__linux__)
                         "linux"
#elif defined(__MACH__)
                         "osx"
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) ||   \
    defined(__DragonFly__)
                         "bsd"
#endif
    );
    (void)lua_pushstring(state, "");
    return 1;
  } catch (...) {
    (void)lua_pushstring(
        state, "An unknown exception was encountered in the get_os function");
    return lua_error(state);
  }
}

// probably shouldn't call it a thunk, but basically just a dummy function that
// doesn't run any commands, nor make any directories, just varifies that there
// is a module there, and that the module is an exe mod
auto Builder::install_exe_thunk(lua_State *state) noexcept -> int {
  LUA_EXPECTED_ARGUMENTS(state, 1, install_exe)
  LUA_ASSERT_FORMAT(state, ret_t, lua_type(state, -1), LUA_TNUMBER,
                    "Expected type of argument to `install_exe` to be of type "
                    "integer, found [%s]",
                    lua_typename(ret_t));

  try {
    auto const mod_idx = lua_tointeger(state, -1);
    lua_pop(state, 1);
    auto const &parent_path = mods.get_module_path(mod_idx).parent_path();

    auto const &exe_mod = mods.module_at(mod_idx);
    if (exe_mod.type != builtins::Module::EXE) {
      throw std::runtime_error(std ::format(
          "module type is not exe, found [{}]",
          static_cast<std::underlying_type_t<builtins::Module::Module_t>>(
              exe_mod.type)));
    }
    return 0;
  } catch (ModuleErr const &e) {
    lua_pushstring(state, e.what().c_str());
    return lua_error(state);
  } catch (DepTreeErr const &e) {
    lua_pushstring(state, e.what().c_str());
    return lua_error(state);
  } catch (pp::Exception const &e) {
    lua_pushstring(state, e.what().c_str());
    return lua_error(state);
  } catch (std::exception const &e) {
    lua_pushstring(state, e.what());
    return lua_error(state);
  } catch (...) {
    lua_pushstring(state, "Unfortunately an error occured");
    return lua_error(state);
  }
}

// probably shouldn't call it a thunk, but basically just a dummy function that
// doesn't run any commands, nor make any directories, just varifies that there
// is a module there, and that the module is a static mod
auto Builder::install_static_thunk(lua_State *state) noexcept -> int {
  LUA_EXPECTED_ARGUMENTS(state, 1, install_exe)
  LUA_ASSERT_FORMAT(
      state, ret_t, lua_type(state, -1), LUA_TNUMBER,
      "Expected type of argument to `install_static` to be of type "
      "integer, found [%s]",
      lua_typename(ret_t));

  try {
    auto const mod_idx = lua_tointeger(state, -1);
    lua_pop(state, 1);
    auto const &parent_path = mods.get_module_path(mod_idx).parent_path();

    auto const &exe_mod = mods.module_at(mod_idx);
    if (exe_mod.type != builtins::Module::STATIC) {
      throw std::runtime_error(std ::format(
          "module type is not static, found [{}]",
          static_cast<std::underlying_type_t<builtins::Module::Module_t>>(
              exe_mod.type)));
    }
    return 0;
  } catch (ModuleErr const &e) {
    lua_pushstring(state, e.what().c_str());
    return lua_error(state);
  } catch (DepTreeErr const &e) {
    lua_pushstring(state, e.what().c_str());
    return lua_error(state);
  } catch (pp::Exception const &e) {
    lua_pushstring(state, e.what().c_str());
    return lua_error(state);
  } catch (std::exception const &e) {
    lua_pushstring(state, e.what());
    return lua_error(state);
  } catch (...) {
    lua_pushstring(state, "Unfortunately an error occured");
    return lua_error(state);
  }
}

// NOTE: this function pushes a new builder object onto the stack, which means
// that if you want to test subprojects you can't really (because the
// LUAMAKE_TEST#n macro won't be included in the compilation)
auto Builder::build_dep_thunk(lua_State *state) noexcept -> int {
  LUA_EXPECTED_ARGUMENTS(state, 1, build_dep);
  LUA_ASSERT_FORMAT(state, ret_t, lua_type(state, -1), LUA_TNUMBER,
                    "Expected type of argument to function "
                    "`build_dep` to be number, found [%s]",
                    lua_typename(ret_t));

  try {
    auto const luamake_path = mods.get_module_path(lua_tointeger(state, -1));
    lua_pop(state, 1);

    if (luaL_dofile(state, luamake_path.c_str()) != LUA_OK) {
      (void)lua_pushfstring(
          state,
          "Unable to run the `luamake.lua` file required, in directory [%s]",
          luamake_path.parent_path().c_str());
      return lua_error(state);
    }

    LUA_ASSERT_FORMAT(state, ret_t, lua_type(state, -1), LUA_TTABLE,
                      "Expected table, found [%s]", lua_typename(ret_t));

    if (auto const build_func_t = lua_getfield(state, -1, "Build");
        build_func_t != LUA_TFUNCTION) {
      (void)lua_pushfstring(
          state,
          "Expected `Build` to have type function when "
          "returned in a table from the `luamake.lua` script at [%s]",
          luamake_path.c_str());
      return lua_error(state);
    }

    builtins::make_builder_thunk(state);
    lua_pushstring(state, luamake_path.parent_path().c_str());
    lua_seti(state, -2, lua_Integer{1});
    if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
      return lua_error(state);
    }
    return 0;
  } catch (std::exception const &e) {
    lua_pushfstring(state, "%s", e.what());
    return lua_error(state);
  } catch (...) {
    lua_pushstring(state, "Unfortunately an error has occured");
    return lua_error(state);
  }

  lua_pushstring(
      state,
      std::format(
          "Unreachable point reached :), please report this. Function [{}]",
          __FUNCTION__)
          .c_str());
  return lua_error(state);
}

auto Runner::run(lua_State *L) noexcept -> int {
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
        lua_typename(t));
    return lua_error(L);
  }

  std::cout << "[" << exe_path << "]\n";
  std::cout.flush();

  OS_CALL(exe_path.data());

  return 0;
}

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
  switch (lua_type(state, -2)) {
  case LUA_TSTRING:
    std::cout << '[' << lua_tolstring(state, -2, nullptr) << "] = ";
    break;
  case LUA_TTABLE:
    std::cout << "[table@" << lua_topointer(state, -2) << "] = ";
    break;
  default:
    std::cout << "[Unknown type@" << lua_topointer(state, -2) << "] = ";
    break;
  }
  dump_impl(state, 1);
  return 0;
}

auto make_builder_obj(lua_State *state) noexcept -> void {
  lua_createtable(state, 1, 9);

  lua_pushcfunction(state, &Builder::clang);
  lua_setfield(state, -2, "clang");

  lua_pushcfunction(state, &Builder::gcc_bare);
  lua_setfield(state, -2, "gcc_bare");

  lua_pushcfunction(state, &Builder::clang_bare);
  lua_setfield(state, -2, "clang_bare");

  lua_pushcfunction(state, &Builder::new_exe);
  lua_setfield(state, -2, "new_exe");

  lua_pushcfunction(state, &Builder::new_static);
  lua_setfield(state, -2, "new_static");

  lua_pushcfunction(state, &Builder::install_exe);
  lua_setfield(state, -2, "install_exe");

  lua_pushcfunction(state, &Builder::install_static);
  lua_setfield(state, -2, "install_static");

  lua_pushcfunction(state, &Builder::build_dep);
  lua_setfield(state, -2, "build_dep");

  lua_pushcfunction(state, &Builder::require);
  lua_setfield(state, -2, "requires");

  lua_pushcfunction(state, &Builder::link_lib);
  lua_setfield(state, -2, "link_lib");

  lua_pushcfunction(state, &Builder::get_os);
  lua_setfield(state, -2, "get_os");

  // this will set Lake[1] = $CWD, which could cause issues, but you should be
  // calling luamake in the same directory with the luamake.lua file in it
  lua_pushstring(state, fs::current_path().c_str());
  lua_seti(state, -2, 1);

  // TODO: add the functions install_dynamic
}

auto make_runner_obj(lua_State *state) noexcept -> void {
  lua_createtable(state, 0, 1);

  lua_pushcfunction(state, &Runner::run);
  lua_setfield(state, -2, "run");
}

auto make_builder_thunk(lua_State *state) noexcept -> void {
  lua_createtable(state, 1, 9);

  lua_pushcfunction(state, &Builder::clang);
  lua_setfield(state, -2, "clang");

  lua_pushcfunction(state, &Builder::gcc_bare);
  lua_setfield(state, -2, "gcc_bare");

  lua_pushcfunction(state, &Builder::clang_bare);
  lua_setfield(state, -2, "clang_bare");

  lua_pushcfunction(state, &Builder::new_exe);
  lua_setfield(state, -2, "new_exe");

  lua_pushcfunction(state, &Builder::new_static);
  lua_setfield(state, -2, "new_static");

  lua_pushcfunction(state, &Builder::install_exe_thunk);
  lua_setfield(state, -2, "install_exe");

  lua_pushcfunction(state, &Builder::install_static_thunk);
  lua_setfield(state, -2, "install_static");

  lua_pushcfunction(state, &Builder::build_dep_thunk);
  lua_setfield(state, -2, "build_dep");

  lua_pushcfunction(state, &Builder::require);
  lua_setfield(state, -2, "requires");

  lua_pushcfunction(state, &Builder::link_lib);
  lua_setfield(state, -2, "link_lib");

  lua_pushcfunction(state, &Builder::get_os);
  lua_setfield(state, -2, "get_os");

  // this will set Lake[1] = $CWD, which could cause issues, but you should be
  // calling luamake in the same directory with the luamake.lua file in it
  lua_pushstring(state, fs::current_path().c_str());
  lua_seti(state, -2, 1);

  // TODO: add the functions install_dynamic
}

LakeModules::LakeModules() noexcept
    : cap(0), size(0), is_compileds(nullptr), compiled_files(nullptr),
      luamake_paths(nullptr), mods(nullptr) {}

auto LakeModules::init(size_t const cap) -> void {
  this->cap = cap;
  size = 0;
  is_compileds = std::make_unique<bool[]>(cap);
  compiled_files = std::make_unique<std::vector<std::string>[]>(cap);
  luamake_paths = std::make_unique<fs::path[]>(cap);
  mods = std::make_unique<Module[]>(cap);

  luamake_paths[0] = fs::current_path() / "luamake.lua";
  is_compileds[0] = false;
  ++size;
}

auto LakeModules::deinit() -> void {
  cap = size = 0;
  is_compileds = nullptr;
  compiled_files = nullptr;
  luamake_paths = nullptr;
  mods = nullptr;
}

auto LakeModules::new_module(fs::path &&path) noexcept -> lua_Integer {
  if (size == cap)
    resize();
  luamake_paths[size] = std::move(path);
  is_compileds[size] = false;

  auto const ret_idx = static_cast<lua_Integer>(size);
  ++size;
  return ret_idx;
}

auto LakeModules::get_module_path(lua_Integer idx) const noexcept -> fs::path {
  return luamake_paths[static_cast<size_t>(idx)];
}

auto LakeModules::emplace_at(lua_Integer idx, Module &&mod) -> void {
  auto const i = static_cast<size_t>(idx);
  mods[i] = std::move(mod);
}

auto LakeModules::module_at(lua_Integer const idx) noexcept -> Module & {
  return mods[static_cast<size_t>(idx)];
}

auto LakeModules::contains(fs::path const &module_name) const noexcept -> int {
  for (auto i = size_t{0}; i < size; ++i) {
    if (luamake_paths[i] == module_name / "luamake.lua") {
      return static_cast<int>(i);
    }
  }
  return -1;
}

auto LakeModules::resize() -> void {
  auto const n_cap = 3 * cap / 2;
  auto n_is_compileds = std::make_unique<bool[]>(n_cap);
  auto n_compiled_files = std::make_unique<std::vector<std::string>[]>(n_cap);
  auto n_luamake_paths = std::make_unique<fs::path[]>(n_cap);
  auto n_mods = std::make_unique<Module[]>(n_cap);

  std::memcpy(n_is_compileds.get(), is_compileds.get(), sizeof(bool) * cap);

  for (auto i = size_t{}; i < cap; ++i)
    n_compiled_files[i] = std::move(compiled_files[i]);
  for (auto i = size_t{}; i < cap; ++i)
    n_luamake_paths[i] = std::move(luamake_paths[i]);
  for (auto i = size_t{}; i < cap; ++i)
    n_mods[i] = std::move(mods[i]);

  is_compileds = std::move(n_is_compileds);
  compiled_files = std::move(n_compiled_files);
  luamake_paths = std::move(n_luamake_paths);
  mods = std::move(n_mods);
}

#ifdef DEBUG
auto LakeModules::dump_paths(std::ostream &out) const noexcept -> void {
  out << "modules.paths = {\n";
  for (auto i = size_t{0}; i < size; ++i) {
    out << "\t[" << i << "][" << luamake_paths[i].string() << "]\n";
  }
  out << "}\n";
  out.flush();
}

auto LakeModules::dump_modules(std::ostream &out) const noexcept -> void {
  out << "modules.mods = {\n";
  for (auto i = size_t{}; i < size; ++i) {
    out << '[' << i << "] {";
    mods[i].display(out);
    out << '}';
  }
  out << "}\n";
  out.flush();
}
#endif // DEBUG

} // namespace builtins
} // namespace luamake
#undef LUA_ASSERT
