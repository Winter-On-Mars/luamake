#ifndef __LUAMAKE_BUILTINS_HPP
#define __LUAMAKE_BUILTINS_HPP

#include "common.hpp"
#include "luamake_pre_ir.hpp"
#include "luamake_spiral.hpp"
#include "luamake_strings.hpp"

#include <filesystem>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

extern "C" {
#include "lua.h"
}

static_assert(LUA_VERSION_NUM == 504);

namespace luamake {
// defined in luamake_thread_pool
struct CompilationPool;

static auto constexpr BUILDER_OBJ = "__luamake_builder";
static auto constexpr RUNNER_OBJ = "__luamake_runner";
static auto constexpr TESTING_MACRO = "__define_testing_macro";

namespace builtins {
auto dump(lua_State *) noexcept -> int;

auto make_builder_obj(lua_State *) noexcept -> void;
auto make_runner_obj(lua_State *) noexcept -> void;
auto make_builder_thunk(lua_State *const) noexcept -> void;

// TODO: add more functions like clang, one for gcc, and a default one that just
// uses the systems cc++ command; also see todo in the function implimenation
// about linking against stdc++, and how we compile c code, bc it's considered
// deprecated to compile c code like it's c++
class Builder final {
  static auto new_exe(lua_State *) noexcept -> int;
  static auto new_static(lua_State *) noexcept -> int;
  static auto install_exe(lua_State *) noexcept -> int;
  static auto install_static(lua_State *) noexcept -> int;

  static auto clang(lua_State *) noexcept -> int;
  static auto gcc_bare(lua_State *) noexcept -> int;
  static auto clang_bare(lua_State *) noexcept -> int;
  static auto require(lua_State *) noexcept -> int;
  static auto link_lib(lua_State *) noexcept -> int;

  static auto get_os(lua_State *) noexcept -> int;

  // used with some of the build commands that want to turn off actually running
  // the compiler
  static auto install_exe_thunk(lua_State *) noexcept -> int;
  static auto install_static_thunk(lua_State *) noexcept -> int;

  friend auto make_builder_obj(lua_State *) noexcept -> void;
  friend auto make_builder_thunk(lua_State *) noexcept -> void;
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

  /**
   * @throws ModuleErr
   */
  Module(Module_t &&type, lua_State *state,
         std::filesystem::path const &) noexcept(false);

  Module(Module &&) noexcept = default;

  Module &operator=(Module &&that) noexcept = default;

  ~Module() noexcept = default;

  Module(Module const &) = delete;
  Module &operator=(Module const &) = delete;

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

    /**
     * @throws DepTreeErr | std::bad_alloc
     */
    [[nodiscard]]
    DepTree(size_t const num_files) noexcept(false);
    ~DepTree() noexcept = default;

    DepTree() noexcept = default;
    DepTree(DepTree const &) = delete;
    DepTree &operator=(DepTree const &) = delete;

    DepTree(DepTree &&) = default;
    DepTree &operator=(DepTree &&) = default;

    /**
     * @throws
     */
    auto gen_dep_tree(Module const &mod, pp::Interpreter &,
                      ModIndex const) noexcept(false) -> void;

#ifdef DEBUG
    // displays the function in a pseudo json format
    auto display(std::ostream &out, unsigned int const depth = 0) const noexcept
        -> void;

    auto dump(std::ostream &out) const noexcept -> void;
#endif // DEBUG

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
    std::unique_ptr<std::vector<uint>[]> deps;
    std::unique_ptr<size_t[]> hashes;

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
    // TODO: rename this to like invalid_idx or something, then we can use 0 as
    // the root index, because that's where the root index *should* be
    static constexpr auto NIL_IDX = static_cast<size_t>(-1);

#ifdef DEBUG
    auto display_impl(std::ostream &out, unsigned int const depth,
                      unsigned int const idx) const noexcept -> void;
#endif // DEBUG

    /**
     * @throws DepTreeErr
     */
    auto append_dep(Module const &, pp::Interpreter &, ModIndex const,
                    std::filesystem::path const &,
                    std::filesystem::path const &, size_t const) -> void;

    friend CompilationPool;
    friend Module;
    friend Builder;
    friend spl::Serializer;
    friend spl::Deserializer;
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
  std::vector<std::filesystem::path> dep_includes;
  std::vector<std::filesystem::path> sys_includes;
  std::vector<std::filesystem::path> linking;
  // TODO: remove this from the module, it should just be on the stack or
  // something
  luamake::pp::Interpreter interpreter;
  // TODO: optimize this :)
  std::string compiler;
  std::string name;
  std::string install_dir;

  Module() noexcept
      : type(), tree(), roots(), headers(), includes(), sys_includes(),
        linking(), interpreter({}, {}), compiler(), name(), install_dir() {}

#ifdef DEBUG
  auto display(std::ostream &) const noexcept -> void;
#endif // DEBUG

  /**
   * @throws CAPI
   */
  auto append_include_paths(std::string_view const) -> void;
  /**
   * @throws CAPI
   */
  [[nodiscard]]
  auto append_predefined_macros(std::string_view const)
      -> std::pair<std::unordered_map<std::string, pp::Macro>,
                   std::unordered_set<std::string>>;

  // TODO: update these to return FixedString
  auto format_includes() const -> std::string;
  auto format_links() const -> std::string;
  static auto parse_compiler_table(lua_State *state) -> std::string;

  friend CompilationPool;
  friend Builder;
  friend spl::Serializer;
  friend spl::Deserializer;
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

  // TODO: maybe have this take ownership, to avoid the copy
  auto new_module(std::filesystem::path const &) noexcept -> void;
  /*
  auto add_mod_to(std::filesystem::path const &, Module &&) noexcept(false)
      -> ModIndex;
      */
  auto get_module_path(ModIndex const) const noexcept -> std::filesystem::path;

  auto emplace_at(ModIndex, Module &&) -> void;

  auto module_at(ModIndex const) noexcept -> Module &;

  auto state_at(ModIndex const) const noexcept -> ModState;
  auto set_state_at(ModIndex const, ModState) noexcept -> void;

  auto add_compiled_file(ModIndex const, std::string &&) noexcept -> void;

  auto get_all_compiled_files(ModIndex const) noexcept -> std::string;

  auto append_module_with_path(std::filesystem::path const &,
                               Module &&) noexcept(false) -> ModIndex;

#ifdef DEBUG
  auto dump_paths(std::ostream &) const noexcept -> void;
  auto dump_modules(std::ostream &) const noexcept -> void;
#endif // DEBUG

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

private:
  auto resize_mods() -> void;
  auto resize_paths() -> void;

  // NOTE: we might need to switch to having a state lock for this class for
  // when we resize, as otherwise we might invalidate some references, we might
  // be able to have cap + 1 mtxs, and then use mtxs[cap] == state mtx,
  // something that could help, or it might be more performant to just have the
  // state mtx inline
  uint mods_cap;
  uint paths_cap;
  // size_t cap;
  // TODO: probably have 2 uints for the num_lake_paths, and for the modules
  uint num_mods;
  uint num_paths;
  // size_t size;
  // TODO: test if it's better to just have all of these in an aos instead of
  // this soa (multiarraylist) that it currently is
  std::unique_ptr<ModState[]> states;
  std::unique_ptr<std::mutex[]> mtxs;
  std::unique_ptr<std::atomic<size_t>[]> remaining_files;
  std::unique_ptr<std::vector<std::string>[]> compiled_files;
  // NOTE: we could switch this to a list<module>, then switch the new_exe
  // function to return a lightuserdata
  std::unique_ptr<Module[]> mods;
  // TODO: switch this to not have the luamake.lua in the path, i.e. just push
  // back the parent path
  std::unique_ptr<std::filesystem::path[]> luamake_paths;

  friend CompilationPool;
};
extern LakeModules mods;

struct CLOptions final {
  bool verbose;
};
extern CLOptions cl_options;
} // namespace builtins
} // namespace luamake

#endif
