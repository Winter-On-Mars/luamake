#include "luamake_builtins.hpp"

#include "common.hpp"
#include "luamake_error.hpp"
#include "luamake_pre_ir.hpp"
#include "luamake_strings.hpp"

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
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
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

namespace fs = std::filesystem;

namespace luamake {
namespace {
using std::pair, std::array, std::string, std::string_view, std::vector,
    std::unordered_map, std::unordered_set;

using uint = unsigned int;

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

struct CAPI final {
  string message;
  explicit CAPI(string const &message) noexcept : message(message) {}
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

struct NonTerminatedPreprocessor final {
  fs::path file;
  explicit NonTerminatedPreprocessor(fs::path const &file) noexcept
      : file(file) {}
  auto error() const noexcept -> string {
    return std::format("Error while processing the preprocessor in file [{}]",
                       file.c_str());
  }
};

using DepTreeErr = std::variant<EmptyFileName, FileDoesNotExist,
                                NonTerminatedString, MalformedInclude, CAPI,
                                MemoryAlloc, NonTerminatedPreprocessor>;

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

using ModuleErr = std::variant<MissingField, UnexpectedType, CAPI>;

struct Module final {
  enum Module_t {
    EXE,
    STATIC,
    DYNAMIC,
  };

  static auto make(Module_t type, lua_State *state)
      -> Result<Module, ModuleErr>;

  auto gen_dep_tree() -> Opt<DepTreeErr>;

  auto constexpr install_dir() const noexcept -> char const * {
    return m.install_dir;
  }
  auto constexpr name() const noexcept -> char const * { return m.name; }
  auto roots() const noexcept -> vector<fs::path> { return m.roots; }
  auto compiler() const noexcept -> std::string { return m.compiler; }
  auto includes() const noexcept -> std::string;
  auto links() const noexcept -> std::string;

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
      auto append_dep(fs::path const &, vector<fs::path> const &,
                      unordered_map<string, ir::Macro> &,
                      unordered_set<string> &, size_t const) -> Opt<DepTreeErr>;
      [[nodiscard]]
      auto get_path(size_t const) const noexcept -> fs::path;
      [[nodiscard]]
      auto find(string_view const) const noexcept
          -> std::tuple<bool, unsigned int, unsigned int>;
      /**
       * @throws std::bad_alloc
       */
      auto resize() noexcept -> void;
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
    std::unordered_map<string, ir::Macro>
        macros; // these are all macros with values
    std::unordered_set<string> defined_macros;
    std::string compiler;
    char const *name;
    char const *install_dir;

    [[remove]]
    auto display(std::ostream &) const noexcept -> void;

    auto append_include_paths(string_view const) noexcept -> Opt<ModuleErr>;
    auto append_predefined_macros(string_view const) noexcept -> Opt<ModuleErr>;
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
      return Err(CAPI(strerror(errno)));
    std::memset(paths, 0, paths_size);

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
  auto const canonical_path = fs::canonical(path);
  if (auto &&[found, start, end] = find(canonical_path.c_str()); found) {
    return std::make_pair(start, end);
  }
  auto start = all_paths.size;
  all_paths.append(canonical_path.string());
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
                                    vector<fs::path> const &includes,
                                    unordered_map<string, ir::Macro> &macros,
                                    unordered_set<string> &def_macros,
                                    size_t const parent_idx)
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

  auto file_string = maybe_file_content.get();
  auto &&[fcontent, fsize] = file_string;

  if (num_files == cap_files) {
    resize();
  }
  auto const this_idx = num_files;
  if (parent_idx != ROOT_IDX)
    deps[parent_idx].push_back(static_cast<unsigned int>(this_idx));
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
        if (auto m_error = append_dep(possible_path, includes, macros,
                                      def_macros, this_idx);
            !m_error.ok()) {
          return m_error;
        }
      }
    }
    // HOL
  }

  types[this_idx] = ftype;
  files[this_idx].start = start;
  files[this_idx].end = end;

  std::cerr << std::format("generating ir for file [{}]\n", dep.c_str());
  auto ir = ir::IR::parse(file_string);
  std::cerr << std::format("interpreting ir for file [{}]\n", dep.c_str());
  auto const files_deps = ir.interpret(macros, def_macros);

  std::cerr << std::format("file deps for [{}]\n", dep.c_str());
  for (auto const &file : files_deps) {
    std::cerr << std::format("{} ", file.c_str());
  }
  std::cerr << '\n';

#if 0
  i this is all we really need to do to get everything up and working again :)
  for (auto const &file : files_deps) {
    if (auto m_error = append_dep(file, includes, macros, def_macros, this_idx);
        !m_error.ok()) {
      return m_error;
    }
  }
#endif

  hashes[this_idx] = hash_fut.get();

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

auto Module::DepTree::M::resize() noexcept -> void {
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
    return Err(CAPI(strerror(errno)));

  auto const _fsize = ftell(file);
  if (_fsize == -1)
    return Err(CAPI(strerror(errno)));

  auto fsize = static_cast<size_t>(_fsize);
  rewind(file);

  auto *fcontent = (char *)malloc(sizeof(char) * fsize + 1);
  if (fcontent == nullptr)
    return Err(CAPI(strerror(errno)));

  if (auto const amount_read = fread(fcontent, sizeof(char), fsize, file);
      amount_read != fsize) {
    free(fcontent);
    return Err(CAPI(strerror(errno)));
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
    out << name << "=" << value.m << ",\n";
  }
  out << "}\n";

  out << "defined_macros = ";
  out << "[" << defined_macros.size() << "]{\n";
  for (auto const &name : defined_macros) {
    out << name << ",\n";
  }
  out << "}\n";

  out << "compiler = " << compiler << '\n';
  out << "name = " << name << '\n';
  out << "install_dir = " << install_dir << '\n';
  out.flush();
}

// TODO: i think i'm not properly handling child procs, so see about fixing it
// in these two functions :)
//
// this function is breaking things :), fix it, figue
// out how pipes work and shit also because we have this now windows support is
// most likely borked :)
auto Module::M::append_include_paths(string_view const compiler) noexcept
    -> Opt<ModuleErr> {
  using Opt = Opt<ModuleErr>;
  using Err = Opt::Err;

  auto _pipes = array<int, 2>{};
  if (pipe(_pipes.data()) == -1) {
    return Err(CAPI(strerror(errno)));
  }
  auto &&[read_pipe, write_pipe] = _pipes;
  auto const pid = vfork();
  if (pid < 0) {
    close(read_pipe);
    close(write_pipe);
    return Err(CAPI(strerror(errno)));
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
      return Err(CAPI(strerror(errno)));
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
      return Err(CAPI(strerror(errno)));
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

    if (amount_read < 0) {
      // idk error happened
      std::cerr << "\terror occured :) when looking at include paths\n";
      std::terminate();
    }

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
  } break;
  }
  return Opt();
}

auto Module::M::append_predefined_macros(string_view const compiler) noexcept
    -> Opt<ModuleErr> {
  using Opt = Opt<ModuleErr>;
  using Err = Opt::Err;

  auto _pipes = array<int, 2>{};
  if (pipe(_pipes.data()) == -1) {
    return Err(CAPI(strerror(errno)));
  }
  auto &&[read_pipe, write_pipe] = _pipes;
  auto const pid = vfork();
  if (pid < 0) {
    close(read_pipe);
    close(write_pipe);
    return Err(CAPI(strerror(errno)));
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
      return Err(CAPI(strerror(errno)));
    }
  } break;
  default: { // in parent proc
    close(write_pipe);
    auto *read_me =
        fdopen(dup(read_pipe), "r"); // idk saw something on stackoverflow
    auto buffer_size = sizeof(char) * size_t{2 << 8};
    auto *buffer = (char *)malloc(buffer_size + 1);
    if (buffer == nullptr)
      return Err(CAPI(strerror(errno)));
    memset(buffer, 0, buffer_size + 1);

    // this seems to work, bc getline returns -1 on EOF so we can't check
    // amount_read to see if there's an error or if we just hit EOF
    errno = 0;

    auto amount_read = getline(&buffer, &buffer_size, read_me);
    for (; amount_read > 0;
         amount_read = getline(&buffer, &buffer_size, read_me)) {
      auto constexpr header = string_view{"#define "};
      if (strncmp(buffer, header.data(), header.size()) != 0) {
        std::cerr << "\tError while processing macros, terminating\n";
        std::cerr << "\tHere is the buffer :) [" << buffer << "]\n";
        fclose(read_me);
        free(buffer);
        close(read_pipe);
        std::terminate();
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
        defined_macros.emplace(buffer + macro_start, buffer + macro_cur - 1);
        continue;
      }

      auto const macro_name =
          string_view{buffer + macro_start, buffer + macro_cur};

      if (buffer[macro_cur] != ' ') {
        std::cerr << "\tdefined macro ended on unexpected character ["
                  << buffer[macro_cur] << "]\n";
        fclose(read_me);
        free(buffer);
        close(read_pipe);
        std::terminate();
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

      macros.emplace(macro_name,
                     ir::Macro{ir::Macro::STRING, string(buffer + macro_start,
                                                         buffer + macro_cur)});
    }

    if (errno != 0) {
      // idk error happened
      std::cerr << "\terror occured :) when looking at macros\n";
      perror("getline");
      std::terminate();
    }

    fclose(read_me);
    free(buffer);
    close(read_pipe);

    // reached EOF

  } break;
  }
  return Opt();
}

auto Module::make(Module_t type, lua_State *state)
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

  ret_t.includes.reserve(10);
  ret_t.includes.emplace_back(".");
  switch (auto const include_t = lua_getfield(state, -5, "include")) {
  case LUA_TTABLE: {
    auto const len = lua_rawlen(state, -1);
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
  ret_t.append_include_paths(ret_t.compiler);
  ret_t.append_predefined_macros(ret_t.compiler);
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
          ret_t.defined_macros.insert(std::move(mac));
        }
      } break;
      default:
        return Err(UnexpectedType("macros[i]", state, LUA_TSTRING, value_t));
      }
      --macros;
    }
    lua_pop(state, static_cast<int>(len));
  } break;
  case LUA_TNIL:
    break;
  default:
    return Err(UnexpectedType("macros", state, LUA_TTABLE, macro_t));
  }

  lua_pop(state, 6);

#if 0
  std::cout << "---displaying---\n";
  ret_t.display(std::cout);
#endif

  return Ok(std::move(ret_t));
}

auto Module::gen_dep_tree() -> Opt<DepTreeErr> {
  using Opt = Opt<DepTreeErr>;

  auto m_m = DepTree::M::make();
  if (m_m == decltype(m_m)::ERR)
    return m_m;

  m.tree = m_m.get();
  try {
    for (auto const &root : m.roots) {
      if (auto m_err = m.tree.m.append_dep(root, m.includes, m.macros,
                                           m.defined_macros, DepTree::ROOT_IDX);
          m_err == decltype(m_err)::ERR) {
        return m_err;
      }
    }
  } catch (ir::Exception const &e) {
    std::cerr << e.what() << '\n';
  } catch (...) {
    std::cerr << "Something was caught.\n";
  }

  return Opt();
}

auto Module::includes() const noexcept -> std::string {
  auto res = string();
  res.reserve(256); // idk random number can def be optimized :)
  for (auto const &path : m.includes) {
    res += std::format(" -I{}", path.string());
  }
  return res;
}

auto Module::links() const noexcept -> std::string {
  // when we add dynamic library support, we'll have to worry about the -L flag
  // and shit
  auto res = string();
  res.reserve(256); // idk random number can def be optimized :)
  for (auto const &path : m.linking) {
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
  // source multiple times, this is probably where that hash set solution would
  // probably make things faster :)
  auto lowest = uint{0};
  remaining_tasks.reserve(sf.m.num_files);
  for (auto i = size_t{}; i < sf.m.num_files; ++i) {
    if (sf.m.types[i] == Module::DepTree::IMPL &&
        sf.m.files[i].start >= lowest) {
      remaining_tasks.push_back(sf.m.get_path(i));
      lowest = sf.m.files[i].start + 1;
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
          Module::DepTree::HEADER) {
        continue;
      }

      auto const include_path = mod->includes();

      auto const invoked_command =
          std::format("{} {} -c {} -o {}/{}.o/{}.o", mod->compiler(),
                      include_path, this_path.c_str(), mod->install_dir(),
                      mod->name(), this_path.stem().c_str());
      std::cout << "[" << invoked_command << "]\n";
      std::cout.flush(); // this actually needs to stay here, something about if
                         // the system command does io operations having an
                         // unflushed stdout can cause problems
      auto const res = system(invoked_command.c_str());
      if (res == 0) {
        auto res_lock = std::unique_lock(result_mtx);
        result += std::format("{}/{}.o/{}.o ", mod->install_dir(), mod->name(),
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
    // pass this value to the _thread_loop function so we don't have to call it
    // in each thread (plus we can make it a string_view so it shouldn't have to
    // worry too much about memory allocations) auto includes = mod.includes();

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

  // TODO: update these functions to throw exceptions
  try {
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
            fs::path(std::format("{}/{}.o", main_mod.install_dir(),
                                 main_mod.name())),
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

#if 0
  auto const actually_compiled_files = Compiler::compile(main_mod);

  // because of the format of `actually_compiled_files` for the best
  // formatting of the command there shouldn't be a space between it and the
  // -o
  auto const invoked_command = std::format(
      "{} -o {}/{} {} {}", main_mod.compiler(), main_mod.install_dir(),
      main_mod.name(), actually_compiled_files, main_mod.links());

  std::cout << "[" << invoked_command << "]\n";
  std::cout.flush();

  if (system(invoked_command.c_str()) != 0) {
    lua_pushfstring(state, "Error compiling [%s]", invoked_command.c_str());
    return lua_error(state);
  } else {
    return 0;
  }
#endif
    return 0;
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
} // namespace builtins
} // namespace luamake
#undef LUA_ASSERT
