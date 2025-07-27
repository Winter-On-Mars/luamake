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
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

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

struct Module final {
  enum Module_t {
    EXE,
    STATIC,
    DYNAMIC,
  };

  static auto make(Module_t &&type, lua_State *state) noexcept -> Module;

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
    fs::path root;
    std::string compiler;
    char const *install_dir;
    // other module deps
  };
  M m;
};

auto Module::make(Module_t &&type, lua_State *state) noexcept -> Module {
  auto ret_t = Module{};
  ret_t.m.type = type;

  lua_getfield(state, -1, "name");
  ret_t.m.name = lua_tolstring(state, -1, nullptr);

  lua_getfield(state, -2, "root");
  ret_t.m.root = lua_tolstring(state, -1, nullptr);

  lua_getfield(state, -3, "compiler");
  ret_t.m.compiler = Module::parse_compiler_table(state);

  lua_getfield(state, -4, "install_dir");
  ret_t.m.install_dir = lua_tolstring(state, -1, nullptr);

  // TODO: update this to record the number of things we push onto the stack
  // to make sure that this doesn't fuck up the stack
  lua_pop(state,
          4); // might cause an issue? just trying to restore the stack

  return ret_t;
}

auto Module::parse_compiler_table(lua_State *state) -> string {
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
  enum SourceFile_t : unsigned char {
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
        if (fs::exists(fs::path(potential_impl + ".cpp"))) {
          auto maybe_impl =
              SourceFile::make(fs::path(potential_impl + ".cpp"), res.m.path);
          switch (maybe_impl) {
          case decltype(maybe_impl)::OK: {
            res.m.deps.emplace_back(new SourceFile(maybe_impl.get()));
          } break;
          case decltype(maybe_impl)::ERR:
            free((void *)fcontent);
            return Err(maybe_impl.err());
          }
        } else if (fs::exists(fs::path(potential_impl + ".c"))) {
          auto maybe_impl =
              SourceFile::make(fs::path(potential_impl + ".c"), res.m.path);
          switch (maybe_impl) {
          case decltype(maybe_impl)::OK: {
            res.m.deps.emplace_back(new SourceFile(maybe_impl.get()));
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
        hash_fut.wait();
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

  ~SourceFile() noexcept {
    for (auto *sf : m.deps) {
      delete sf;
    }
  }

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

    serialize_impl(outfile);
    outfile.flush();
  }

  // TODO: we're just assuming that the path is well constructed
  // so add some error handling to this function
  [[nodiscard(
      "We spent all this time deserializing you better use the result")]]
  static auto deserialize(fs::path const &path) noexcept -> SourceFile {
    auto file = File(path, File::READ | File::BINARY);
    if (file == nullptr) {
      std::cerr << "unable to open serialization file [" << path << "]\n";
      std::terminate();
    }

    return SourceFile::deserialize_impl(file);
  }

  auto path() const noexcept -> fs::path { return m.path; }
  auto deps() const noexcept -> vector<SourceFile *> { return m.deps; }
  auto type() const noexcept -> SourceFile_t { return m.type; }

  [[nodiscard]]
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

private:
  struct M final {
    SourceFile_t type;
    fs::path path;
    vector<SourceFile *> deps;
    size_t hash;
    // TODO:
    // std::thread hashing_thread;
    // std::thread dep_analyzer_thread;
  } m;

  static auto analyze_dep(fs::path const &path, fs::path const &parent,
                          FILE *file, char const *fcontent) noexcept
      -> Result<decltype(SourceFile::M::deps), SourceFileErr> {
    using Ok = Result<decltype(SourceFile::M::deps), SourceFileErr>::Ok;
    using Err = Result<decltype(SourceFile::M::deps), SourceFileErr>::Err;

    auto res = vector<SourceFile *>();
    res.reserve(4);

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
              auto *_sf = new SourceFile(sf.get());
              res.emplace_back(_sf);
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
          std::cerr << "unknown char [" << *ch << "]\n";
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

    auto *fcontent = (char *)malloc(sizeof(char) * fsize + 1);
    if (fcontent == nullptr)
      return Err(CFileAPIError(strerror(errno)));

    if (auto const amount_read = fread(fcontent, sizeof(char), fsize, file);
        amount_read != fsize)
      return Err(CFileAPIError(strerror(errno)));
    fcontent[fsize] = 0;
    return Ok(std::make_pair(fcontent, fsize));
  }

  auto serialize_impl(File &file) const noexcept -> void {
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

  static auto deserialize_impl(File &file) noexcept -> SourceFile {
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

  SourceFile() = default;
  SourceFile(SourceFile::M &&m) noexcept : m(std::move(m)) {}
  friend Result<SourceFile, SourceFileErr>;
};

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

[[nodiscard]]
auto compile(Module const &mod, SourceFile const *sf) noexcept -> string {
  auto res = string();

  auto tp = CompilationPool(mod);
  tp.add_task(sf);
  tp.run();
  res = tp.get();

  return res;
}

auto install_exe(lua_State *state) -> int {
  using Result = Result<SourceFile, SourceFileErr>;
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
    auto exe_root = maybe_exe_root.get();

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
    auto const actually_compiled_files = compile(main_mod, &exe_root);

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
  } break;
  case Result::ERR: {
    auto const msg = std::visit([](auto &&e) { return e.error() + '\n'; },
                                maybe_exe_root.err());

    lua_pushstring(state, msg.c_str());
    return lua_error(state);
  } break;
  }
}

auto install_static(lua_State *L) -> int {
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
} // namespace

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
