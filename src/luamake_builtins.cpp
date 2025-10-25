#include "luamake_builtins.hpp"

#include "common.hpp"
#include "luamake_pre_ir.hpp"
#include "luamake_strings.hpp"

// #define DEBUG

extern "C" {
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
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
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
  if ((A) != (B)) {                                                            \
    lua_pushstring((L), (ERROR));                                              \
    return lua_error((L));                                                     \
  }

#define LUA_ASSERT_FORMAT(L, name, A, B, fmt, ...)                             \
  if (auto const name = (A); (name) != (B)) {                                  \
    lua_pushfstring((L), fmt, __VA_ARGS__);                                    \
    return lua_error((L));                                                     \
  }

namespace fs = std::filesystem;

// TODO: reorder things in this namespace bc things are kind of all over the
// place
namespace luamake {
namespace {
using std::pair, std::array, std::string, std::string_view, std::vector,
    std::unordered_map, std::unordered_set;

// TODO: support vectorization, it should really speed things up when
// serializing the DepTree
struct Serializer final {
  Serializer() noexcept;
  ~Serializer() noexcept = default;

  // there should be a better way of doing this, that allows for partial
  // specialization if we use structs with an overloaded operator(), but idk how
  // to really do that
  template <class T>
    requires(std::is_trivial_v<std::remove_cv_t<T>>)
  inline auto serialize(T) noexcept -> Serializer & = delete;

  // used for string_view, and other view types that are not technically
  // trivial, but are trivially copyable
  template <class T>
    requires(std::is_trivially_copyable_v<std::remove_cv_t<T>> &&
             !std::is_trivial_v<std::remove_cv_t<T>>)
  inline auto serialize(T const) noexcept -> Serializer & = delete;

  template <class T>
    requires(!std::is_trivial_v<std::remove_cv_t<T>> &&
             !std::is_trivially_copyable_v<std::remove_cv_t<T>>)
  inline auto serialize(T const &) noexcept -> Serializer & = delete;

  auto buffer() noexcept -> std::pair<size_t, std::unique_ptr<u8[]>> {
    // this probably doesn't do what i want it to do :)
    return std::make_pair(size, std::move(buf));
  }

  // this might not be right
  Serializer(Serializer &&) = default;
  Serializer &operator=(Serializer &&) = default;

  Serializer(Serializer const &) = delete;
  Serializer &operator=(Serializer const &) = delete;

private:
  // TODO: either add a func overload or another function to resize_atleast,
  // that takes in a number of bytes that we need to resize the value to at
  // least
  auto resize() noexcept -> void;
  size_t cap;
  size_t size;
  std::unique_ptr<u8[]> buf;
};

struct Deserializer final {
  Deserializer(File &);
  ~Deserializer() noexcept = default;

  template <class T> inline auto deserialize() noexcept -> T = delete;

  // this might not be right
  Deserializer(Deserializer &&) = default;
  Deserializer &operator=(Deserializer &&) = default;

  Deserializer() noexcept = delete;
  Deserializer(Serializer const &) = delete;
  Deserializer &operator=(Serializer const &) = delete;

private:
  size_t size;
  size_t cur;
  std::unique_ptr<u8[]> buf;
};

// should be page size, this should be enough to never have to resize, but we
// still need the resize funcs for completeness, might also be a good idea to
// change this to be platform dependant, just on my system page size is 4kb
Serializer::Serializer() noexcept
    : cap(1 << 12), size(0), buf(std::make_unique<u8[]>(cap)) {}

auto Serializer::resize() noexcept -> void {
  auto const new_cap = 3 * cap / 2;
  auto new_buf = std::make_unique<u8[]>(new_cap);

  std::memcpy(new_buf.get(), buf.get(), cap);

  buf = std::move(new_buf);
  cap = new_cap;
}

Deserializer::Deserializer(File &file) : size(0), cur(0), buf(nullptr) {
  auto &&[tmp_size, tmp_buf] = file.dump_content();
  if (tmp_buf == nullptr) {
    throw std::runtime_error("Unable to read file content");
  }
  size = tmp_size;
  buf = std::move(tmp_buf);
}

auto constexpr lua_typename(int const type) -> char const * {
  if (type <= 0 || LUA_NUMTYPES <= type) {
    throw std::runtime_error("type is out of range to be a lua type");
  }

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
  }
  unreachable();
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

struct Module final {
  enum class Module_t : u8 {
    EXE,
    STATIC,
    DYNAMIC,
  };
  using enum Module_t;

  /**
   * @throws ModuleErr
   */
  Module(Module_t &&type, lua_State *state) noexcept(false);

  /**
   * @throws
   */
  auto gen_dep_tree() noexcept(false) -> void;

  auto serialize(fs::path const &path) const -> void;
  [[nodiscard(
      "We spent all this time deserializing you better use the result")]]
  static auto deserialize(fs::path const &path)
      -> std::variant<Module, std::string>;

  Module(Module &&) = default;
  ~Module() noexcept = default;

  Module(Module const &) = delete;
  Module &operator=(Module const &) = delete;

  auto operator==(Module const &) const noexcept -> bool;

  // this is kinda stupid i'm not gonna lie, but this is the only
  // way i can think to have DepTree be able to reference Module and vice versa
  // without having to worry about pointer indirection
  struct DepTree final {
    enum class SourceFile_t : u8 {
      IMPL,
      HEADER,
      SYSTEM,
      MISC,
    };

    /**
     * @throws DepTreeErr | std::bad_alloc
     */
    [[nodiscard]]
    DepTree(size_t const num_files = 8) noexcept(false);

    DepTree(DepTree const &) = delete;
    DepTree &operator=(DepTree const &) = delete;

    DepTree(DepTree &&) = default;
    DepTree &operator=(DepTree &&) = default;

    ~DepTree() noexcept = default;

#ifdef DEBUG
    // displays the function in a pseudo json format
    auto display(std::ostream &out, unsigned int const depth = 0) const noexcept
        -> void;
#endif // DEBUG

    [[nodiscard]]
    static auto determine_file_type(fs::path &&ext) noexcept -> SourceFile_t {
      if (ext == ".cpp" || ext == ".cxx" || ext == ".cc" || ext == ".c") {
        return SourceFile_t::IMPL;
      }
      if (ext == ".hpp" || ext == ".hxx" || ext == ".hh" || ext == ".h") {
        return SourceFile_t::HEADER;
      }
      return SourceFile_t::MISC;
    }

  private:
    // a parallel array for all of the source files
    // NOTE: this could be pushed further, and we could have a
    // memory allocator as a part of this struct, then just
    // clearing the memory allocator would act as the destructor
    // TODO: if performance becomes an issue, it might be good to switch this to
    // a hash set for the `find` function
    OwnedString all_paths;
    size_t num_files;
    size_t cap_files;
    std::unique_ptr<SourceFile_t[]> types;
    std::unique_ptr<StringViews[]> files;
    std::unique_ptr<vector<unsigned int>[]> deps;
    std::unique_ptr<size_t[]> hashes;

    [[nodiscard]]
    auto append_path(fs::path const &) -> std::pair<bool, StringViews>;
    [[nodiscard]]
    auto get_path(size_t const) const noexcept -> fs::path;
    [[nodiscard]]
    auto find(string_view const) const noexcept -> std::pair<bool, StringViews>;
    /**
     * @throws std::bad_alloc
     */
    auto resize() noexcept(false) -> void;
    /**
     * @throws std::bad_alloc
     */
    auto reserve(size_t) noexcept(false) -> void;

    // basically making the assumption that a project isn't gonna have
    // size_t.max files in it, idk if that's even physically possible
    // so this *seems like* a valid assumption
    static constexpr auto ROOT_IDX = static_cast<size_t>(-1);

    /**
     * @throws DepTreeErr
     */
    static auto get_file_content(FILE *file) noexcept(false) -> FixedString;

#ifdef DEBUG
    auto display_impl(std::ostream &out, unsigned int const depth,
                      unsigned int const idx) const noexcept -> void;
#endif // DEBUG

    friend Compiler;
    friend CompilationPool;
    friend Module;
    friend Serializer;
    friend Deserializer;
  };

  Module_t type;
  // it *might* be a cool idea to have this as a union of
  // vector<fs::path> and fs::path for better domain modeling, but
  // unions are a bit of a pain to work with in c++
  DepTree tree;
  vector<fs::path> roots;
  vector<fs::path> includes;
  vector<fs::path> linking;
  std::unordered_map<string, pp::Macro>
      macros; // these are all macros with values
  std::unordered_set<string> def_macros;
  pp::Interpreter interpreter;
  // TODO: switch these over to either all be std::string, or have them be
  // factored out into a OwnedString + StringViews structure
  // for de/serialization, these just being char const * makes thins a bit of a
  // pain, bc we rely on them being interned in the lua gc, but when they are
  // deserialized, they cannot be interned, so we're going to leak this memory
  // :)
  std::string compiler;
  char const *name;
  char const *install_dir;

  Module() noexcept
      : type(), tree(), roots(), includes(), linking(), macros(), def_macros(),
        interpreter(macros, def_macros), compiler(), name(nullptr),
        install_dir(nullptr) {}

#ifdef DEBUG
  auto display(std::ostream &) const noexcept -> void;
#endif // DEBUG

  /**
   * @throws CAPI
   */
  auto append_include_paths(string_view const) -> void;
  /**
   * @throws CAPI
   */
  auto append_predefined_macros(string_view const) -> void;
  /**
   * @throws DepTreeErr
   */
  auto append_dep(fs::path const &, size_t const) -> void;

  // TODO: update these to return FixedString
  auto format_includes() const -> string;
  auto format_links() const -> string;
  static auto parse_compiler_table(lua_State *state) -> string;

  friend CompilationPool;
  friend Compiler;
  friend Serializer;
  friend Deserializer;
};

template <>
inline auto Serializer::serialize<unsigned int>(unsigned int i) noexcept
    -> Serializer & {
  if (size >= cap) {
    resize();
  }
  std::memcpy(buf.get() + size, &i, sizeof(decltype(i)));
  size += sizeof(decltype(i));
  return *this;
};

template <>
inline auto Serializer::serialize<Module::Module_t>(Module::Module_t t) noexcept
    -> Serializer & {
  if (size >= cap) {
    resize();
  }
  std::memcpy(buf.get() + size, &t, sizeof(decltype(t)));
  size += sizeof(decltype(t));
  return *this;
};

template <>
inline auto Serializer::serialize<Module::DepTree::SourceFile_t>(
    Module::DepTree::SourceFile_t t) noexcept -> Serializer & {
  if (size >= cap) {
    resize();
  }
  std::memcpy(buf.get() + size, &t, sizeof(decltype(t)));
  size += sizeof(decltype(t));
  return *this;
};

template <>
inline auto Serializer::serialize<StringViews>(StringViews str) noexcept
    -> Serializer & {
  if (size >= cap) {
    resize();
  }
  std::memcpy(buf.get() + size, &str, sizeof(decltype(str)));
  size += sizeof(decltype(str));
  return *this;
};

template <>
inline auto Serializer::serialize<size_t>(size_t x) noexcept -> Serializer & {
  if (size + sizeof(decltype(x)) >= cap) {
    resize();
  }
  std::memcpy(buf.get() + size, &x, sizeof(decltype(x)));
  size += sizeof(decltype(x));
  return *this;
};

template <>
inline auto Serializer::serialize<char const *>(char const *c_str) noexcept
    -> Serializer & {
  auto const str_len = strlen(c_str);
  serialize(str_len);

  if (size + str_len >= cap) {
    resize();
  }

  std::memcpy(buf.get() + size, c_str, str_len);
  size += str_len;
  return *this;
};

template <>
inline auto Serializer::serialize<std::string>(std::string const &str) noexcept
    -> Serializer & {
  auto const str_size = str.size();
  serialize(str_size);

  if (size + str_size >= cap) {
    resize();
  }
  std::memcpy(buf.get() + size, str.c_str(), str_size);
  size += str_size;

  return *this;
};

template <>
inline auto Serializer::serialize<OwnedString>(OwnedString const &str) noexcept
    -> Serializer & {
  auto const str_size = str.size;
  serialize(str_size);

  if (size + str_size >= cap) {
    resize();
  }
  std::memcpy(buf.get() + size, str.buffer, str_size);
  size += str_size;

  return *this;
};

template <>
inline auto Serializer::serialize<string_view>(string_view const str) noexcept
    -> Serializer & {
  auto const str_size = str.size();
  serialize(str_size);

  if (size + str_size >= cap) {
    resize();
  }

  std::memcpy(buf.get() + size, str.data(), str_size);
  size += str_size;

  return *this;
};

template <>
inline auto Serializer::serialize<fs::path>(fs::path const &path) noexcept
    -> Serializer & {
  auto const str = path.string();
  return serialize(str);
};

template <>
inline auto
Serializer::serialize<vector<fs::path>>(vector<fs::path> const &vec) noexcept
    -> Serializer & {
  auto const vec_size = vec.size();
  serialize(vec_size);

  for (auto &&ent : vec) {
    serialize(ent);
  }

  return *this;
};

template <>
inline auto Serializer::serialize<vector<unsigned int>>(
    vector<unsigned int> const &vec) noexcept -> Serializer & {
  auto const vec_size = vec.size();
  serialize(vec_size);

  for (auto &&ent : vec) {
    serialize(ent);
  }

  return *this;
};

template <>
inline auto Serializer::serialize<std::unordered_set<string>>(
    std::unordered_set<string> const &set) noexcept -> Serializer & {
  auto const set_size = set.size();
  serialize(set_size);

  for (auto &&ent : set) {
    serialize(ent);
  }
  return *this;
};

template <>
inline auto Serializer::serialize<std::unordered_map<string, pp::Macro>>(
    std::unordered_map<string, pp::Macro> const &map) noexcept -> Serializer & {
  auto const map_size = map.size();
  serialize(map_size);

  for (auto &&[key, val] : map) {
    serialize(key);
    serialize(val);
  }

  return *this;
};

template <>
inline auto
Serializer::serialize<Module::DepTree>(Module::DepTree const &tree) noexcept
    -> Serializer & {
  serialize(tree.all_paths);
  serialize(tree.num_files);
  for (auto i = size_t{}; i < tree.num_files; ++i) {
    serialize(tree.types[i]);
  }
  for (auto i = size_t{}; i < tree.num_files; ++i) {
    serialize(tree.files[i]);
  }
  for (auto i = size_t{}; i < tree.num_files; ++i) {
    serialize(tree.deps[i]);
  }
  for (auto i = size_t{}; i < tree.num_files; ++i) {
    serialize(tree.hashes[i]);
  }
  return *this;
};

template <>
inline auto Serializer::serialize<Module>(Module const &mod) noexcept
    -> Serializer & {
  return serialize(mod.type)
      .serialize(mod.tree)
      .serialize(mod.roots)
      .serialize(mod.includes)
      .serialize(mod.linking)
      .serialize(mod.macros)
      .serialize(mod.def_macros)
      .serialize(mod.compiler)
      .serialize(mod.name)
      .serialize(mod.install_dir);
}

template <>
inline auto Deserializer::deserialize<unsigned int>() noexcept -> unsigned int {
  unsigned int i = 0;
  std::memcpy(&i, buf.get() + cur, sizeof(decltype(i)));
  cur += sizeof(decltype(i));
  return i;
};

template <>
inline auto Deserializer::deserialize<Module::Module_t>() noexcept
    -> Module::Module_t {
  auto mod = Module::Module_t{};
  std::memcpy(&mod, buf.get() + cur, sizeof(decltype(mod)));
  cur += sizeof(decltype(mod));
  return mod;
};

template <>
inline auto Deserializer::deserialize<Module::DepTree::SourceFile_t>() noexcept
    -> Module::DepTree::SourceFile_t {
  auto sf_t = Module::DepTree::SourceFile_t{};
  std::memcpy(&sf_t, buf.get() + cur, sizeof(decltype(sf_t)));
  cur += sizeof(decltype(sf_t));
  return sf_t;
};

template <>
inline auto Deserializer::deserialize<StringViews>() noexcept -> StringViews {
  auto sv = StringViews{};
  std::memcpy(&sv, buf.get() + cur, sizeof(decltype(sv)));
  cur += sizeof(decltype(sv));
  return sv;
};

template <> inline auto Deserializer::deserialize<size_t>() noexcept -> size_t {
  auto x = size_t{};
  std::memcpy(&x, buf.get() + cur, sizeof(decltype(x)));
  cur += sizeof(decltype(x));
  return x;
};

template <>
inline auto Deserializer::deserialize<char const *>() noexcept -> char const * {
  auto const str_len = deserialize<size_t>();
  auto *str = (char const *)malloc(str_len);
  std::memcpy((void *)str, buf.get() + cur, str_len);
  cur += str_len;
  return str;
};

template <>
inline auto Deserializer::deserialize<std::string>() noexcept -> std::string {
  auto const str_len = deserialize<size_t>();
  auto str = std::string();
  str.resize(str_len);
  // this is technically dangerous, but bc we string.resize it *should* be fine
  std::memcpy((void *)str.c_str(), buf.get() + cur, str_len);
  cur += str_len;
  return str;
};

template <>
inline auto Deserializer::deserialize<OwnedString>() noexcept -> OwnedString {
  auto const str_len = deserialize<size_t>();
  auto str = OwnedString();
  str.buffer = (char *)malloc(str_len);
  std::memcpy(str.buffer, buf.get() + cur, str_len);
  str.size = str.capacity = str_len;
  cur += str_len;
  return str;
};

// TODO: this function will also leak memory like the char const * one :)
template <>
inline auto Deserializer::deserialize<string_view>() noexcept -> string_view {
  // LEAK
  auto *tmp_str = deserialize<char const *>();
  auto str = string_view(tmp_str);

  return str;
};

template <>
inline auto Deserializer::deserialize<fs::path>() noexcept -> fs::path {
  auto str = deserialize<std::string>();
  return fs::path(str);
};

template <>
inline auto Deserializer::deserialize<vector<fs::path>>() noexcept
    -> vector<fs::path> {
  auto const vec_size = deserialize<size_t>();
  auto res = vector<fs::path>(vec_size);
  for (auto i = size_t{}; i < vec_size; ++i) {
    res[i] = deserialize<fs::path>();
  }
  return res;
};

template <>
inline auto Deserializer::deserialize<vector<unsigned int>>() noexcept
    -> vector<unsigned int> {
  auto const vec_size = deserialize<size_t>();
  auto res = vector<unsigned int>(vec_size);
  for (auto i = size_t{}; i < vec_size; ++i) {
    res[i] = deserialize<unsigned int>();
  }
  return res;
};

template <>
inline auto Deserializer::deserialize<std::unordered_set<string>>() noexcept
    -> std::unordered_set<string> {
  auto const set_size = deserialize<size_t>();
  auto set = std::unordered_set<string>();
  set.reserve(set_size);
  for (auto i = size_t{}; i < set_size; ++i) {
    set.insert(deserialize<string>());
  }
  return set;
};

template <>
inline auto
Deserializer::deserialize<std::unordered_map<string, pp::Macro>>() noexcept
    -> std::unordered_map<string, pp::Macro> {
  auto const map_size = deserialize<size_t>();
  auto map = std::unordered_map<string, pp::Macro>();

  for (auto i = size_t{}; i < map_size; ++i) {
    auto key = deserialize<string>();
    auto val = deserialize<string>();
    map[key] = val;
  }
  return map;
};

template <>
inline auto Deserializer::deserialize<Module::DepTree>() noexcept
    -> Module::DepTree {
  auto tree = Module::DepTree();
  tree.all_paths = deserialize<OwnedString>();
  tree.num_files = deserialize<size_t>();

  tree.reserve(tree.num_files);

  for (auto i = size_t{}; i < tree.num_files; ++i) {
    tree.types[i] = deserialize<Module::DepTree::SourceFile_t>();
  }
  for (auto i = size_t{}; i < tree.num_files; ++i) {
    tree.files[i] = deserialize<StringViews>();
  }
  for (auto i = size_t{}; i < tree.num_files; ++i) {
    tree.deps[i] = deserialize<vector<unsigned int>>();
  }
  for (auto i = size_t{}; i < tree.num_files; ++i) {
    tree.hashes[i] = deserialize<size_t>();
  }
  return tree;
};

template <> inline auto Deserializer::deserialize<Module>() noexcept -> Module {
  auto mod = Module();
  mod.type = deserialize<decltype(Module::type)>();
  mod.tree = deserialize<decltype(Module::tree)>();
  mod.roots = deserialize<decltype(Module::roots)>();
  mod.includes = deserialize<decltype(Module::includes)>();
  mod.linking = deserialize<decltype(Module::linking)>();
  mod.macros = deserialize<decltype(Module::macros)>();
  mod.def_macros = deserialize<decltype(Module::def_macros)>();
  mod.compiler = deserialize<decltype(Module::compiler)>();
  mod.name = deserialize<decltype(Module::name)>();
  mod.install_dir = deserialize<decltype(Module::install_dir)>();
  return mod;
}

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

  auto const file_string = DepTree::get_file_content(file);
  auto &&[fcontent, fsize] = file_string;

  auto hash_fut = std::async(std::launch::async, [fcontent, fsize]() {
    return fnv1a(fsize, fcontent);
  });
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

  auto const files_deps = interpreter.interpret(file_string);

  for (auto const &file : files_deps) {
    auto const maybe_file = [&]() -> std::optional<fs::path> {
      for (auto const &include : includes) {
        auto const p = fs::canonical(include / file);
#ifdef DEBUG
        std::cerr << std::format("p.parent_path()/p.stem() = [{}], "
                                 "dep.parent_path()/dep.stem() = [{}]\n",
                                 (p.parent_path() / p.stem()).string(),
                                 (dep.parent_path() / dep.stem()).string());
#endif
        if (!fs::exists(p)) {
          continue;
        }
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

auto Module::serialize(fs::path const &path) const -> void {
  auto outfile = File(path, File::WRITE | File::BINARY);
  if (outfile == nullptr) {
    return;
  }
  auto serializer = Serializer();
  auto &&[size, buffer] = serializer.serialize(*this).buffer();

  outfile.write(buffer.get(), size, 1);
  outfile.flush();
}

auto Module::deserialize(fs::path const &path)
    -> std::variant<Module, std::string> {
  auto file = File(path, File::READ | File::BINARY);
  if (file == nullptr) {
    return std::format("unable to open serialization file [{}]", path.c_str());
  }

  auto deserializer = Deserializer(file);
  return deserializer.deserialize<Module>();
}

auto Module::operator==(Module const &that) const noexcept -> bool {
  if (type != that.type)
    return false;

  // tree
  if (tree.all_paths.view() != that.tree.all_paths.view())
    return false;

  if (tree.num_files != that.tree.num_files)
    return false;

  for (auto i = size_t{}; i < tree.num_files; ++i) {
    if (tree.types[i] != that.tree.types[i])
      return false;
  }
  for (auto i = size_t{}; i < tree.num_files; ++i) {
    if (tree.files[i].start != that.tree.files[i].start &&
        tree.files[i].end != that.tree.files[i].end)
      return false;
  }
  for (auto i = size_t{}; i < tree.num_files; ++i) {
    if (tree.deps[i].size() != that.tree.deps[i].size())
      return false;

    for (auto j = size_t{}; j < tree.deps[i].size(); ++j) {
      if (tree.deps[i][j] != that.tree.deps[i][j])
        return false;
    }
  }
  for (auto i = size_t{}; i < tree.num_files; ++i) {
    if (tree.hashes[i] != that.tree.hashes[i])
      return false;
  }

  // rest of the class
  if (macros != that.macros)
    return false;

  if (def_macros != that.def_macros)
    return false;

  if (compiler != that.compiler)
    return false;

  if (strcmp(name, that.name) != 0)
    return false;

  if (strcmp(install_dir, that.install_dir) != 0)
    return false;

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

  out << "includes= [";
  std::for_each(includes.begin(), includes.end(), _display);
  out << "]\n";

  out << "linking = [";
  std::for_each(linking.begin(), linking.end(), _display);
  out << "]\n";

  out << "macros = {\n";
  for (auto &&[name, value] : macros) {
    out << name << "=" << value << ",\n";
  }
  out << "}\n";

  out << "defined_macros = ";
  out << "[" << def_macros.size() << "]{\n";
  for (auto const &name : def_macros) {
    out << name << ",\n";
  }
  out << "}\n";

  out << "compiler = " << compiler << '\n';
  out << "name = " << name << '\n';
  out << "install_dir = " << install_dir << '\n';
  out.flush();
}
#endif // DEBUG

// TODO: i think i'm not properly handling child procs, so see about fixing it
// in these two functions :)
//
// this function is breaking things :), fix it, figue
// out how pipes work and shit also because we have this now windows support is
// most likely borked :)
auto Module::append_include_paths(string_view const compiler) -> void {
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
    // TODO: calling into std::format here seems to be causing a memory leak
    // i think b/c we call into execl, which replaces this exe with the called
    // one that it results in a memory leak, so we have to find a better way of
    // handling the child proc
    auto const command_string =
        std::format("{} -v -c -xc++ /dev/null -o {}/luamake_null.o",
                    string_view{compiler.data(), compiler.find(' ')},
                    fs::temp_directory_path().c_str());

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
    close(read_pipe);

    if (amount_read < 0)
      throw CAPI(strerror(errno));

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
      }
      start_path = skip_ws(end_path);
    }
    (void)waitpid(pid, nullptr, WNOHANG);
  } break;
  }
}

auto Module::append_predefined_macros(string_view const compiler) -> void {
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
    // TODO: calling into std::format here seems to be causing a memory leak
    // i think b/c we call into execl, which replaces this exe with the called
    // one that it results in a memory leak, so we have to find a better way of
    // handling the child proc
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

  } break;
  }
}

Module::Module(Module_t &&type, lua_State *state)
    : type(type), tree(), roots(), includes(), linking(), macros(),
      def_macros(), interpreter(macros, def_macros), compiler(), name(nullptr),
      install_dir(nullptr) {
  switch (auto const name_t = lua_getfield(state, -1, "name")) {
  case LUA_TSTRING:
    name = lua_tolstring(state, -1, nullptr);
    break;
  case LUA_TNIL:
    throw MissingField("name");
  default:
    throw UnexpectedType("name", LUA_TSTRING, name_t);
  }

  switch (type) {
  case EXE:
    roots.reserve(1);
    switch (auto const root_t = lua_getfield(state, -2, "root")) {
    case LUA_TSTRING:
      roots.push_back(
          fs::canonical(fs::path(lua_tolstring(state, -1, nullptr))));
      break;
    case LUA_TNIL:
      throw MissingField("root");
    default:
      throw UnexpectedType("root", LUA_TSTRING, root_t);
    }
    break;
  case STATIC: {
    switch (auto const root_t = lua_getfield(state, -2, "roots")) {
    case LUA_TTABLE: {
      auto const num_roots = lua_rawlen(state, -1);
      auto const roots_idx = lua_absindex(state, -1);
      roots.reserve(num_roots);
      for (lua_pushnil(state); lua_next(state, roots_idx) != 0;) {
        if (auto const value_t = lua_type(state, -1); value_t != LUA_TSTRING) {
          throw UnexpectedType("roots[i]", LUA_TSTRING, value_t);
        }
        roots.push_back(
            fs::canonical(fs::path(lua_tolstring(state, -1, nullptr))));
        lua_pop(state, 1);
      }
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

  switch (auto const compiler_t = lua_getfield(state, -3, "compiler")) {
  case LUA_TTABLE:
    compiler = Module::parse_compiler_table(state);
    break;
  case LUA_TNIL:
    throw MissingField("compiler");
  default:
    throw UnexpectedType("compiler", LUA_TTABLE, compiler_t);
  }

  switch (auto const install_dir_t = lua_getfield(state, -4, "install_dir")) {
  case LUA_TSTRING:
    install_dir = lua_tolstring(state, -1, nullptr);
    break;
  case LUA_TNIL:
    throw MissingField("install_dir");
  default:
    throw UnexpectedType("install_dir", LUA_TSTRING, install_dir_t);
  }

  includes.reserve(10);
  for (auto const &root : roots) {
    auto found = false;
    for (auto const &include : includes) {
      if (include == fs::canonical(root.parent_path())) {
        found = true;
      }
    }
    if (!found) {
      includes.push_back(fs::canonical(root.parent_path()));
    }
  }
  switch (auto const include_t = lua_getfield(state, -5, "include")) {
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
  append_include_paths(compiler);
  append_predefined_macros(compiler);
  switch (auto const linking_t = lua_getfield(state, -6, "linking")) {
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

  switch (auto const macro_t = lua_getfield(state, -7, "macros")) {
  case LUA_TTABLE: {
    auto const len = lua_rawlen(state, -1);
    auto macros = -1;
    for (auto i = 1; i <= len; ++i) {
      switch (auto const value_t = lua_geti(state, macros, i)) {
      case LUA_TSTRING: {
        auto mac = string(lua_tolstring(state, -1, nullptr));
        if (mac.find('=') != mac.npos) {
          // TODO: parse macro being set to value
        } else {
          def_macros.insert(std::move(mac));
        }
      } break;
      default:
        throw UnexpectedType("macros[i]", LUA_TSTRING, value_t);
      }
      --macros;
    }
    lua_pop(state, static_cast<int>(len));
  } break;
  case LUA_TNIL:
    break;
  default:
    throw UnexpectedType("macros", LUA_TTABLE, macro_t);
  }

  lua_pop(state, 6);

#ifdef DEBUG
  // std::cout << "---displaying---\n";
  // display(std::cout);
#endif
}

auto Module::gen_dep_tree() -> void {
  // we probably don't need this, b/c the constructor will be called when we
  // originally construct this module
  // [[tree = DepTree();]]
  for (auto const &root : roots) {
    append_dep(root, DepTree::ROOT_IDX);
  }

#ifdef DEBUG
  tree.display(std::cout);
#endif
}

auto Module::format_includes() const -> std::string {
  auto res = string();
  res.reserve(256); // idk random number can def be optimized :)
  for (auto const &path : includes) {
    res += std::format(" -I{}", path.string());
  }
  return res;
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
          "Incorrect type in `warnings` table"); // TODO: update this to
                                                 // include the found type
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
// TODO: update this to take advantage of the current layout for DepTree
// i.e. relying on DepTree.types to determine what to compile
struct CompilationPool final {
  CompilationPool(size_t num_threads) noexcept;

  ~CompilationPool() noexcept;

  auto init(Module const *const mod) noexcept -> void;

private:
  auto add_task(Module::DepTree const &) -> void;
  auto run() -> void;
  auto busy() noexcept -> bool;
  auto get() -> string;
  auto constexpr done() const noexcept -> bool { return mod == nullptr; }

  CompilationPool() = delete;
  CompilationPool(CompilationPool &&) = delete;
  CompilationPool &operator=(CompilationPool &&) = delete;
  CompilationPool(CompilationPool const &) = delete;
  CompilationPool &operator=(CompilationPool const &) = delete;

  auto _thread_loop() noexcept -> void;

  vector<std::thread> workers;
  vector<fs::path> remaining_tasks;
  std::mutex task_mtx;

  string result = string();
  std::mutex result_mtx;

  Module const *mod = nullptr;

  friend Compiler;
};

CompilationPool::CompilationPool(size_t num_threads) noexcept {
  workers.reserve(num_threads);
}

CompilationPool::~CompilationPool() noexcept {
  {
    auto lock = std::unique_lock(task_mtx);
    mod = nullptr;
  }
  for (auto &thread : workers) {
    if (thread.joinable()) // ?
      thread.join();
  }
}

auto CompilationPool::init(Module const *const mod) noexcept -> void {
  this->mod = mod;
}

auto CompilationPool::run() -> void {
  for (auto i = size_t{0}; i < workers.capacity(); ++i) {
    workers.emplace_back([this]() { _thread_loop(); });
  }
  for (auto &worker : workers) {
    worker.join();
  }
}

auto CompilationPool::add_task(Module::DepTree const &sf) -> void {
  auto lock = std::unique_lock(task_mtx);
  // this is a really hacky solution to fix the issues of compiling the same
  // source multiple times, this is probably where that hash set solution
  // would probably make things faster :)
  auto lowest = len_t{0};
  remaining_tasks.reserve(sf.num_files);
  for (auto i = size_t{}; i < sf.num_files; ++i) {
    if (sf.types[i] == Module::DepTree::SourceFile_t::IMPL &&
        sf.files[i].start >= lowest) {
      remaining_tasks.push_back(sf.get_path(i));
      lowest = sf.files[i].start + 1;
    }
  }
}

auto CompilationPool::busy() noexcept -> bool {
  // std::this_thread::sleep_for(std::chrono::nanoseconds(100));
  auto pool_busy = true;
  {
    auto lock = std::unique_lock(task_mtx);
    pool_busy = !remaining_tasks.empty();
  }
  return pool_busy;
}

auto CompilationPool::_thread_loop() noexcept -> void {
  try {
    while (true) {
      auto guard = std::unique_lock(task_mtx);

      if (done() || remaining_tasks.empty())
        return;

      auto this_path = remaining_tasks.back();
      remaining_tasks.pop_back();
      guard.unlock();

      // TODO: see if we can remove this, i think we're only pushing back the
      // IMPL files anyways so there's no need to check this here
      if (Module::DepTree::determine_file_type(this_path.extension()) ==
          Module::DepTree::SourceFile_t::HEADER) {
        continue;
      }

      auto const include_path = mod->format_includes();

      auto const invoked_command =
          std::format("{} {} -c {} -o {}/{}.o/{}.o", mod->compiler,
                      include_path, this_path.c_str(), mod->install_dir,
                      mod->name, this_path.stem().c_str());
      std::cout << "[" << invoked_command << "]\n";
      std::cout.flush(); // this actually needs to stay here, something about
                         // if the system command does io operations having an
                         // unflushed stdout can cause problems
      auto const res = system(invoked_command.c_str());
      if (res == 0) {
        auto res_lock = std::unique_lock(result_mtx);
        result += std::format("{}/{}.o/{}.o ", mod->install_dir, mod->name,
                              this_path.stem().c_str());
      }
    }
  } catch (std::exception const &e) {
    std::cerr << "caught exception [" << e.what()
              << "] in _thread_loop on thread [" << std::this_thread::get_id()
              << "]";
    return;
  }
}

auto CompilationPool::get() -> string {
  while (busy()) {
    /* wait */
  }

  // idk if this is needed?
  for (auto &worker : workers) {
    if (worker.joinable())
      worker.join();
  }

  auto lock = std::unique_lock(result_mtx);

  return result;
}

struct Compiler final {
  [[nodiscard]]
  static auto compile(Module const &mod) noexcept -> string {
    auto res = string();
    auto pool = CompilationPool(std::thread::hardware_concurrency() - 1);
    // pass this value to the _thread_loop function so we don't have to call
    // it in each thread (plus we can make it a string_view so it shouldn't
    // have to worry too much about memory allocations) auto includes =
    // mod.includes();

    pool.init(&mod);
    pool.add_task(mod.tree);
    pool.run();
    res = pool.get();
    return res;
  }
};

auto new_exe(lua_State *state) noexcept -> int {
  (void)lua_pushstring(state, "new_exe not impl");
  return lua_error(state);
}

auto new_static(lua_State *state) noexcept -> int {
  (void)lua_pushstring(state, "new_static not impl");
  return lua_error(state);
}

auto install_exe(lua_State *state) noexcept -> int {
  auto num_args = lua_gettop(state);
  if (num_args != 1) {
    lua_pushstring(state, "Too many arguments.");
    return lua_error(state);
  }

  LUA_ASSERT_FORMAT(state, ret_t, lua_type(state, -1), LUA_TTABLE,
                    "Expected type of argument to `install_exe` to be of type "
                    "table, found [%s]",
                    lua_typename(ret_t));

  try {
    auto main_mod = Module(Module::EXE, state);

    // TODO: update these functions to throw exceptions
    auto ec = std::error_code{};
    if (fs::create_directories(
            fs::path(
                std::format("{}/{}.o", main_mod.install_dir, main_mod.name)),
            ec);
        ec) {
      std::cerr << ec.message() << '\n';
      lua_pushstring(state, "Unable to create directory");
      return lua_error(state);
    }
    ec.clear();

    if (fs::create_directories(
            fs::path(std::format("{}/__luamake_cache", main_mod.install_dir)),
            ec);
        ec) {
      std::cerr << ec.message() << '\n';
      lua_pushstring(state, "Unable to create directory");
      return lua_error(state);
    }
    ec.clear();
    auto const path = fs::path(std::format(
        "{}/__luamake_cache/{}.cache", main_mod.install_dir, main_mod.name));

    auto maybe_cached_mod_fut = std::async(
        std::launch::async, [&path]() { return Module::deserialize(path); });

    main_mod.gen_dep_tree();

#ifdef DEBUG
    std::cout << "uncached mod\n";
    main_mod.display(std::cout);
    std::cout << "---\n";
#endif // DEBUG

    auto maybe_cached_mod = maybe_cached_mod_fut.get();
    /* compare the current mod with the cached mod */
    switch (maybe_cached_mod.index()) {
    case 0: {
      auto const &cached_mod = std::get<Module>(maybe_cached_mod);
#ifdef DEBUG
      std::cout << "cached mod\n";
      cached_mod.tree.display(std::cout);
      cached_mod.display(std::cout);
      std::cout << "---\n";
#endif // DEBUG
      if (main_mod == cached_mod) {
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

    auto const actually_compiled_files = Compiler::compile(main_mod);

    // because of the format of `actually_compiled_files` for the best
    // formatting of the command there shouldn't be a space between it and the
    // -o
    auto const invoked_command = std::format(
        "{} -o {}/{} {} {}", main_mod.compiler, main_mod.install_dir,
        main_mod.name, actually_compiled_files, main_mod.format_links());

    std::cout << "[" << invoked_command << "]\n";
    std::cout.flush();

    if (system(invoked_command.c_str()) != 0) {
      lua_pushfstring(state, "Error compiling [%s]", invoked_command.c_str());
      return lua_error(state);
    } else {
      // only serialize if everything went right, need to figure out a better
      // way to do this, so that we only serialize what was actually
      // successfully compiled for incrimental builds and stuff
      main_mod.serialize(path);

      return 0;
    }
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

auto install_static(lua_State *state) noexcept -> int {
  auto const num_args = lua_gettop(state);
  if (num_args != 1) {
    lua_pushstring(state, "Too many arguments");
    return lua_error(state);
  }

  LUA_ASSERT_FORMAT(state, ret_t, lua_type(state, -1), LUA_TTABLE,
                    "Expected type of argument to `install_static` "
                    "to be of type table, found [%s]",
                    lua_typename(ret_t));

  try {
    auto static_mod = Module(Module::STATIC, state);

    auto ec = std::error_code{};
    if (fs::create_directories(fs::path(
            std::format("{}/{}.o", static_mod.install_dir, static_mod.name)));
        ec) {
      lua_pushfstring(state, "Unable to create directory\n\t[%s]",
                      ec.message().c_str());
      return lua_error(state);
    }
    ec.clear();

    static_mod.gen_dep_tree();

    auto const compiled_files = Compiler::compile(static_mod);
    auto const invoked_command =
        std::format("ar crs {}/lib{}.a {}", static_mod.install_dir,
                    static_mod.name, compiled_files);

    std::cout << '[' << invoked_command << "]\n";
    std::cout.flush();
    if (system(invoked_command.c_str()) != 0) {
      lua_pushstring(
          state, std::format("Error compiling [{}]", invoked_command).c_str());
      return lua_error(state);
    } else {
      return 0;
    }
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
        lua_typename(t));
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
                      lua_typename(arg_t));
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
                               lua_typename(include_table_t))
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
                    lua_typename(roots_t))
            .c_str());
    return lua_error(state);
  }
  auto const roots = lua_absindex(state, -1);
  // i'm just assuming that this table is an array, and that the user won't
  // mess that up
  auto init_include_tbl_len = lua_rawlen(state, -2);
  for (lua_pushnil(state); lua_next(state, roots); ++init_include_tbl_len) {
    LUA_ASSERT_FORMAT(
        state, value_t, lua_type(state, -1), LUA_TSTRING,
        "Expected value type in roots table to be a [string], found %s",
        lua_typename(value_t));
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
                               lua_typename(linking_tbl_t))
                       .c_str());
    return lua_error(state);
  }
  // get slib.name and slib.install_dir for adding the archive to be linked
  LUA_ASSERT_FORMAT(
      state, name_t, lua_getfield(state, slib_idx, "name"), LUA_TSTRING,
      "Expected type of [static library].name to be [string] found [%s]",
      lua_typename(name_t));
  LUA_ASSERT_FORMAT(
      state, install_dir_t, lua_getfield(state, slib_idx, "install_dir"),
      LUA_TSTRING,
      "Expected type of [static library].root to be [string] found [%s]",
      lua_typename(install_dir_t));
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
        std::cout << lua_typename(key_t);
        break;
      }

      std::cout << ':';
      dump_impl(state, depth + 1);

      lua_pop(state, 1);
    }
    std::cout << indents << '}';
  } break;
  default:
    std::cout << "Unable to display type of " << lua_typename(type);
    break;
  }
  std::cout << '\n';
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

auto require(lua_State *state) noexcept -> int {
  auto const num_args = lua_gettop(state);
  if (num_args != 1) {
    lua_pushstring(state, "Expected one argument to the clang function");
    return lua_error(state);
  }
  (void)lua_pushstring(state, "require not impl");
  return lua_error(state);
}
} // namespace

namespace builtins {
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

auto make_builder_obj(lua_State *state) noexcept -> void {
  lua_createtable(state, 0, 6);

  lua_pushcfunction(state, clang);
  lua_setfield(state, -2, "clang");

  lua_pushcfunction(state, new_exe);
  lua_setfield(state, -2, "new_exe");

  lua_pushcfunction(state, new_static);
  lua_setfield(state, -2, "new_static");

  lua_pushcfunction(state, install_exe);
  lua_setfield(state, -2, "install_exe");

  lua_pushcfunction(state, install_static);
  lua_setfield(state, -2, "install_static");

  lua_pushcfunction(state, link_static);
  lua_setfield(state, -2, "link_static");

  // TODO: add the functions install_dynamic
}

auto make_runner_obj(lua_State *state) noexcept -> void {
  lua_createtable(state, 0, 1);

  lua_pushcfunction(state, run);
  lua_setfield(state, -2, "run");
}

auto make_lake_obj(lua_State *state) noexcept -> void {
  lua_createtable(state, 0, 1);

  lua_pushcfunction(state, require);
  lua_setfield(state, -2, "require");
}
} // namespace builtins
} // namespace luamake
#undef LUA_ASSERT
