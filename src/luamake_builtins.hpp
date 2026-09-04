#ifndef __LUAMAKE_BUILTINS_HPP
#define __LUAMAKE_BUILTINS_HPP

#include "common.hpp"
#include "luamake_allocator.hpp"
#include "luamake_pre_ir.hpp"
#include "luamake_spiral.hpp"
#include "luamake_strings.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

extern "C" {
#include "lua/lua.h"
}

static_assert(LUA_VERSION_NUM == 504);

namespace luamake {
// defined in luamake_thread_pool
struct CompilationPool;

static auto constexpr BUILDER_OBJ = "__luamake_builder";
static auto constexpr RUNNER_OBJ = "__luamake_runner";
static auto constexpr TESTING_MACRO = "__define_testing_macro";

namespace builtins {
// forward declare, used for friend annotations
struct LakeModules;
auto dump(lua_State *) noexcept -> int;

auto make_builder_obj(lua_State *) noexcept -> void;
auto make_runner_obj(lua_State *) noexcept -> void;
auto make_builder_dummy(lua_State *const) noexcept -> void;

class Builder final {
  static auto new_exe(lua_State *) noexcept -> int;
  static auto new_static(lua_State *) noexcept -> int;
  static auto new_dynamic(lua_State *) noexcept -> int;

  static auto install_exe(lua_State *) noexcept -> int;
  static auto install_static(lua_State *) noexcept -> int;
  static auto install_dynamic(lua_State *) noexcept -> int;
  // used when building programs that use other build systems, brings their
  // system into ours
  static auto install_dep(lua_State *) noexcept -> int;

  static auto clang(lua_State *) noexcept -> int;
  static auto gcc(lua_State *) noexcept -> int;
  static auto gcc_bare(lua_State *) noexcept -> int;
  static auto clang_bare(lua_State *) noexcept -> int;

  static auto cmake(lua_State *) noexcept -> int;

  static auto require(lua_State *) noexcept -> int;
  static auto link_lib(lua_State *) noexcept -> int;

  // TODO: we could probably move get_os to be a member on the build_ctx, rather
  // than a function, idk about the build_type, but it's worth looking at if
  // that's possible
  static auto get_os(lua_State *) noexcept -> int;
  static auto build_type(lua_State *) noexcept -> int;

  static auto install_exe_dummy(lua_State *) noexcept -> int;
  static auto install_static_dummy(lua_State *) noexcept -> int;
  static auto install_dynamic_dummy(lua_State *) noexcept -> int;
  static auto install_dep_dummy(lua_State *) noexcept -> int; // ?

  friend auto make_builder_obj(lua_State *) noexcept -> void;
  friend auto make_builder_dummy(lua_State *) noexcept -> void;
};

class Runner final {
  static auto run(lua_State *) noexcept -> int;
  friend auto make_runner_obj(lua_State *) noexcept -> void;
};

struct ModIndex final {
  uint files;
  uint mods;

  constexpr ModIndex() noexcept = default;
  constexpr ModIndex(uint files, uint mods) noexcept
      : files(files), mods(mods) {}

  explicit constexpr ModIndex(lua_Integer const i) noexcept
      : files(static_cast<uint>(i >> 32)), mods(static_cast<uint>(i)) {}

  // idk this can almost certainly be improved, but because it's constexpr i
  // assume the optimizer will just make this go away
  explicit constexpr operator lua_Integer() const noexcept {
    auto res = lua_Integer{0};
    res |= files;
    res <<= 32;
    res |= mods;
    return res;
  }

  friend auto operator<<(std::ostream &, ModIndex const) noexcept
      -> std::ostream &;

  static auto constexpr not_found = static_cast<uint>(-1);
};
static_assert(sizeof(ModIndex) == sizeof(lua_Integer),
              "really dumb, but we need to push this onto the lua stack as an "
              "\"integer\"");

static_assert([]() -> bool {
  auto constexpr idx = ModIndex(0, 1);
  auto constexpr lint = static_cast<lua_Integer>(idx);
  static_assert(lint == lua_Integer{0x1});
  return true;
}());
static_assert([]() -> bool {
  auto constexpr idx = ModIndex(6, 1);
  auto constexpr lint = static_cast<lua_Integer>(idx);
  static_assert(lint == lua_Integer{0x0000000600000001});
  return true;
}());

static_assert([]() -> bool {
  auto constexpr idx = lua_Integer{1};
  auto constexpr mod_idx = ModIndex(idx);
  static_assert(mod_idx.files == 0);
  static_assert(mod_idx.mods == 1);
  return true;
}());
static_assert([]() -> bool {
  auto constexpr idx = lua_Integer{0x0000000200000004};
  auto constexpr mod_idx = ModIndex(idx);
  static_assert(mod_idx.files == 2);
  static_assert(mod_idx.mods == 4);
  return true;
}());

struct MacroStorage {
  std::filesystem::path fpath;
  pp::MacroMap macros;
  pp::StringSet defs;
};

// TODO: add exported header field, and probably refactor this to be a tagged
// union to discriminate between exe and library type modules
// TODO: rewrite how the include files are processed to have a system header
// section and an include header section, to be used later when compiling +
// generating compile_commands.json
struct Module final {
  enum class Module_t : u8 {
    EXE,
    STATIC,
    DYNAMIC,
  };
  using enum Module_t;

  // @throws std::runtime_error
  Module(Module_t &&type, lua_State *state,
         std::filesystem::path const &) noexcept(false);

  Module(Module &&) noexcept = default;

  Module &operator=(Module &&that) noexcept = default;

  ~Module() noexcept = default;

  Module(Module const &) = delete;
  Module &operator=(Module const &) = delete;

  // NOTE: doesn't compare the trees bc we want to take a diff of them for
  // incrimental builds
  auto operator==(Module const &) const noexcept -> bool;

  // this is kinda stupid i'm not gonna lie, but this is the only
  // way i can think to have DepTree be able to reference Module and vice versa
  // without having to worry about pointer indirection
  // TODO: have the DepTree depend on the LakeModules, where all of these paths
  // are relative to said lakemodule
  struct DepTree final {
    enum class SourceFile_t : u8 {
      IMPL,
      HEADER,
      SYSTEM,
      MISC,
    };

    // @throws std::runtime_error | std::bad_alloc
    [[nodiscard]]
    DepTree(size_t const num_files) noexcept(false);
    ~DepTree() noexcept = default;

    DepTree() noexcept = default;
    DepTree(DepTree const &) = delete;
    DepTree &operator=(DepTree const &) = delete;

    DepTree(DepTree &&) = default;
    DepTree &operator=(DepTree &&) = default;

    // @throws std::runtime_error
    auto gen_dep_tree(Module const &mod, pp::Interpreter &,
                      ModIndex const) noexcept(false) -> void;

#ifdef DEBUG_MOD
    // displays the function in a pseudo json format
    auto display(std::ostream &out, unsigned int const depth = 0) const noexcept
        -> void;

    auto dump(std::ostream &out) const noexcept -> void;
#endif // DEBUG_MOD

    [[nodiscard]]
    static auto determine_file_type(std::filesystem::path &&ext) noexcept
        -> SourceFile_t {
      if (ext == ".cpp" || ext == ".cxx" || ext == ".cc" || ext == ".c") {
        return SourceFile_t::IMPL;
      }
      if (ext == ".hpp" || ext == ".hxx" || ext == ".hh" || ext == ".h") {
        return SourceFile_t::HEADER;
      }
      return SourceFile_t::MISC;
    }

    [[nodiscard]]
    auto append_path(std::filesystem::path const &) -> StringViews;
    [[nodiscard]]
    auto get_path(size_t const) const noexcept -> std::filesystem::path;
    [[nodiscard]]
    auto find(std::string_view const) const noexcept -> size_t;

    [[nodiscard]]
    constexpr auto size() const noexcept -> size_t {
      return num_files;
    }

    auto vectorize() const -> std::vector<std::string_view>;
    auto is_empty() const noexcept -> bool;

  private:
    // a parallel array for all of the source files
    // NOTE: this could be pushed further, and we could have a
    // memory allocator as a part of this struct, then just
    // clearing the memory allocator would act as the destructor
    // TODO: if performance becomes an issue, it might be good to switch this to
    // a hash set for the `find` function
    // TODO: (Winter-On-Mars) perf check if having the paths relative to the CWD
    // is faster than having them relative to their respective luamake.lua file
    // (current)
    OwnedString all_paths;
    size_t num_files;
    size_t cap_files;
    std::unique_ptr<SourceFile_t[]> types;
    std::unique_ptr<StringViews[]> files;
    std::unique_ptr<std::vector<uint>[]> deps;
    std::unique_ptr<size_t[]> hashes;

    // @throws std::bad_alloc
    auto resize() noexcept(false) -> void;
    // @throws std::bad_alloc
    auto reserve(size_t) noexcept(false) -> void;

    // basically making the assumption that a project isn't gonna have
    // size_t.max files in it, idk if that's even physically possible
    // so this *seems like* a valid assumption
    static constexpr auto NIL_IDX = static_cast<size_t>(-1);

#ifdef DEBUG_MOD
    auto display_impl(std::ostream &out, unsigned int const depth,
                      unsigned int const idx) const noexcept -> void;
#endif // DEBUG_MOD

    // @throws std::runtime_error
    auto append_dep(Module const &, pp::Interpreter &, ModIndex const,
                    std::filesystem::path const &,
                    std::filesystem::path const &, size_t const) -> void;

    friend LakeModules;
    friend CompilationPool;
    friend Module;
    friend Builder;
    friend spl::Serializer<DepTree>;
    friend spl::Deserializer<DepTree>;
  };

  // NOTE: we could probably use the empty space in the vector<fs::path> headers
  // field, where if headers.len == 0, then we have to be an executable
  // we would need a way to encode the difference between static and dynamic
  // library's still
  // moreover, we can optimize this struct more by having the strings all be
  // held in a giant string, then taking string_view into said string

  // TODO: make this private and have an api for it or whatever
  Module_t type;
  DepTree tree;
  std::vector<std::filesystem::path> roots;
  std::vector<std::filesystem::path> headers;
  std::vector<std::filesystem::path> includes;
  std::vector<std::filesystem::path> sys_includes;
  std::vector<std::filesystem::path> links;
  std::vector<std::filesystem::path> sys_links;
  // NOTE: we need to seperate this into the macros that are predefined, and
  // those that are then defined in files, as an example we could have something
  // like this
  //   A
  //  / \
  // B   C
  // if B defines `FOO`, and then C checks for `FOO`, then we should use a
  // globally defined `FOO` and/or a `FOO` that was passed through the macros
  // variable in the config rather than B's `FOO`
  // TODO: move all macros to just be a part of a single hash map, treating
  // `#define FOO` to just be `#define FOO ` with it's value being the empty
  // string, because that's what happens when we need to do textual substitution
  // then we can have a per file diff, in case the file #undef's a macro and/or
  // redefines a macro
  std::unique_ptr<pp::Interpreter> interpreter;
  pp::StringMap<MacroStorage> macro_cache;
  // TODO: optimize this :)
  std::string compiler;
  std::string name;
  std::string install_dir;

  static auto from_external(Module_t &&, std::string &&,
                            std::vector<std::filesystem::path> &&) -> Module;

  Module() noexcept
      : type(), tree(), roots(), headers(), includes(), sys_includes(), links(),
        sys_links(), interpreter({}, {}), compiler(), name(), install_dir() {}

#ifdef DEBUG_MOD
  auto display(std::ostream &) const noexcept -> void;
#endif // DEBUG_MOD

  // @throws std::runtime_error
  auto append_include_paths(std::string const &) -> void;
  // @throws std::runtime_error
  [[nodiscard]]
  auto append_predefined_macros(std::string const &)
      -> std::pair<pp::MacroMap, pp::StringSet>;

  // TODO: update these to return FixedString
  auto format_includes() const -> std::string;
  auto format_links() const -> std::string;
  static auto parse_compiler_table(lua_State *state) -> std::string;

  friend CompilationPool;
  friend Builder;
  friend spl::Serializer<Module>;
  friend spl::Deserializer<Module>;
};

// EXPL: the ModIndex struct is used to index into this, the top 32 bits index
// into luamake_paths, while the bottom 32 bits index into everything else.
// there will always be more mods/ everything else compared to the
// luamake_paths, so the cap and size variables refer to the total number of
// possible things that can be indexed into, we'll just not keep track of the
// total number of luamake_paths, meaning that if something happens, the user
// *could* index into uninitialized memory :), idk something we could fix but i
// want to get this working first
// this does lead to a fun issues where we have to worry about which mutex
// refers to what, but for now we won'tworry about it
struct LakeModules final {
  enum class ModState {
    uninitialized,
    compiled,
    ready_for_final_compile,
    error
  };

  LakeModules() noexcept;

  auto init(size_t const cap = 4) -> void;
  auto deinit() -> void;

  auto has_module_at(ModIndex const) const noexcept -> bool;

  // TODO: maybe have this take ownership, to avoid the copy
  auto new_module(std::filesystem::path const &) noexcept -> void;

  auto get_module_path(ModIndex const) const noexcept -> std::filesystem::path;

  auto emplace_at(ModIndex, Module &&) -> void;

  auto module_at(ModIndex const) noexcept -> Module &;

  auto state_at(ModIndex const) const noexcept -> ModState;
  auto set_state_at(ModIndex const, ModState) noexcept -> void;

  auto add_compiled_file(ModIndex const, std::string &&) noexcept -> void;

  auto get_all_compiled_files(ModIndex const) noexcept -> std::string;

  auto append_module_with_path(std::filesystem::path const &,
                               Module &&) noexcept(false) -> ModIndex;

  // ok to return a string_view bc the strings will be alive in the
  // mod.tree.all_paths field
  auto get_tree_diff(ModIndex const, Module::DepTree const &,
                     Module::DepTree const &) -> std::vector<std::string_view>;

#ifdef DEBUG_MOD
  auto dump_paths(std::ostream &) const noexcept -> void;
  auto dump_modules(std::ostream &) const noexcept -> void;
#endif // DEBUG_MOD

  struct Iterator final {
    constexpr Iterator(ModIndex const idx, Module *mods) noexcept
        : idx(idx), mods(mods) {}
    auto operator++() noexcept -> Iterator & {
      ++idx.mods;
      return *this;
    }
    auto operator*() noexcept -> Module const & { return mods[idx.mods]; }
    auto constexpr operator==(Iterator const that) const noexcept -> bool {
      return idx.mods == that.idx.mods;
    }

  private:
    ModIndex idx;
    Module *mods;
  };

  auto begin() const noexcept -> Iterator {
    return Iterator(ModIndex(), mods.get());
  }
  auto end() const noexcept -> Iterator {
    return Iterator(ModIndex(0, num_mods), nullptr);
  }

  auto init_allocator() noexcept -> void;
  auto get_allocator() noexcept -> allocator::Page &;

private:
  auto resize_mods() -> void;
  auto resize_paths() -> void;

  // NOTE: we might be able to avoid having so many mutexs if we have some kind
  // of std::atomic<T*>(?)
  uint mods_cap;
  uint num_mods;
  std::unique_ptr<ModState[]> states;
  std::unique_ptr<std::mutex[]> mtxs;
  std::unique_ptr<std::atomic<size_t>[]> remaining_files;
  std::unique_ptr<std::vector<std::string>[]> compiled_files;
  std::unique_ptr<Module[]> mods;
  // TODO: switch this to not have the luamake.lua in the path, i.e. just push
  // back the parent path
  uint paths_cap;
  uint num_paths;
  std::unique_ptr<std::filesystem::path[]> luamake_paths;

  allocator::Page arena;

  friend CompilationPool;
};
extern LakeModules mods;

// TODO: actually use these, i think only verbose is currently being used, and
// it's not even being used that well :(
// TODO: add cli parsing to all of these (i know there's those cli annotations
// but they don't do anything, that would be great to set up as a part of the
// build system? to have some command line parsing generated automatically)
struct CLOptions final {
  enum class BuildType : u8 { def, dbg = def, rel, dbg_w_rel, min_rel };
  enum class ProjectType : u8 { executable, static_, dynamic };
  enum class LoggingLevel : u8 { none, terminal, file };
  // [[cli("--verbose", "-v")]]
  bool verbose = false;
  // [[cli("--everything")]]
  bool clean_everything = false;
  // [[cli("--build-type=%s")]] ??
  BuildType built_t = BuildType::def;
  // [[cli("")]] ??
  ProjectType proj_t = CLOptions::ProjectType::executable;
  // [[cli("--no-cache")]]
  bool cache = true; // when set to false it means we don't cache, used when
                     // building a project that you're __sure__ you're only
                     // going to build once (like if you got a binary from the
                     // package manager), skips de/serialization
  // [[cli("--num-threads=%zu")]] ??
  size_t num_threads = std::thread::hardware_concurrency();
};
extern CLOptions cl_options;
} // namespace builtins
} // namespace luamake
#endif
