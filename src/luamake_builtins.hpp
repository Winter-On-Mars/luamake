#ifndef __LUAMAKE_BUILTINS_HPP
#define __LUAMAKE_BUILTINS_HPP

#include "common.hpp"
#include "luamake_pre_ir.hpp"
#include "luamake_strings.hpp"

#include <filesystem>
#include <memory>
#include <ostream>
#include <thread>
#include <variant>
#include <vector>

extern "C" {
#include "lua.h"
}

static_assert(LUA_VERSION_NUM == 504);

namespace luamake {
namespace builtins {
auto dump(lua_State *state) noexcept -> int;

auto make_builder_obj(lua_State *state) noexcept -> void;
auto make_runner_obj(lua_State *state) noexcept -> void;

class Builder final {
  static auto new_exe(lua_State *) noexcept -> int;
  static auto new_static(lua_State *) noexcept -> int;
  static auto install_exe(lua_State *) noexcept -> int;
  static auto install_static(lua_State *) noexcept -> int;
  static auto build_dep(lua_State *) noexcept -> int;
  static auto clang(lua_State *) noexcept -> int;
  static auto require(lua_State *) noexcept -> int;
  static auto link_lib(lua_State *) noexcept -> int;
  static auto compile_commands_json(lua_State *) noexcept -> int;
  friend auto make_builder_obj(lua_State *) noexcept -> void;
};

class Runner final {
  static auto run(lua_State *) noexcept -> int;
  friend auto make_runner_obj(lua_State *) noexcept -> void;
};

struct CompilationPool;
struct Compiler;

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

  /**
   * @throws
   */
  auto gen_dep_tree() noexcept(false) -> void;

  auto serialize(std::filesystem::path const &path) const -> void;
  [[nodiscard(
      "We spent all this time deserializing you better use the result")]]
  static auto deserialize(std::filesystem::path const &path)
      -> std::variant<Module, std::string>;

  Module(Module &&) noexcept = default;

  Module &operator=(Module &&that) noexcept = default;

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
    std::unique_ptr<std::vector<unsigned int>[]> deps;
    std::unique_ptr<size_t[]> hashes;

    [[nodiscard]]
    auto append_path(std::filesystem::path const &)
        -> std::pair<bool, StringViews>;
    [[nodiscard]]
    auto get_path(size_t const) const noexcept -> std::filesystem::path;
    [[nodiscard]]
    auto find(std::string_view const) const noexcept
        -> std::pair<bool, StringViews>;
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
    friend Builder;
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
  /**
   * @throws DepTreeErr
   */
  auto append_dep(std::filesystem::path const &, size_t const) -> void;

  // TODO: update these to return FixedString
  auto format_includes() const -> std::string;
  auto format_links() const -> std::string;
  static auto parse_compiler_table(lua_State *state) -> std::string;

  friend CompilationPool;
  friend Compiler;
  friend Builder;
};

// sort of a thread pool like structure that is just for compiling
// TODO: update this to take advantage of the current layout for DepTree
// i.e. relying on DepTree.types to determine what to compile
// TODO: rewrite the system so that this can run in the background while we
// build the dep tree for other modules, and just queue jobs into this as needed
struct CompilationPool final {
  CompilationPool(size_t num_threads) noexcept;

  ~CompilationPool() noexcept;

  auto init(Module const *const mod) noexcept -> void;

private:
  auto add_task(Module::DepTree const &) -> void;
  auto run() -> void;
  auto busy() noexcept -> bool;
  auto get() -> std::string;
  auto constexpr done() const noexcept -> bool { return mod == nullptr; }

  CompilationPool() = delete;
  CompilationPool(CompilationPool &&) = delete;
  CompilationPool &operator=(CompilationPool &&) = delete;
  CompilationPool(CompilationPool const &) = delete;
  CompilationPool &operator=(CompilationPool const &) = delete;

  auto _thread_loop() noexcept -> void;

  std::vector<std::thread> workers;
  std::vector<std::filesystem::path> remaining_tasks;
  std::mutex task_mtx;

  std::string result = std::string();
  std::mutex result_mtx;

  Module const *mod = nullptr;

  friend Compiler;
};

// TODO: just turn this into a function ig
struct Compiler final {
  [[nodiscard]]
  static auto compile(Module const &mod) noexcept -> std::string;
};

// TODO: add an explicit init and deinit function to this so that we can have
// better control over the lifetime of this object
struct LakeModules final {
  LakeModules() noexcept;

  auto init(size_t const cap = 4) -> void;
  auto deinit() -> void;

  auto new_module(std::filesystem::path &&) noexcept -> lua_Integer;
  auto get_module_path(lua_Integer) const noexcept -> std::filesystem::path;

  auto emplace_at(lua_Integer, Module &&) -> void;

  auto construct_module_at(lua_Integer, Module::Module_t, lua_State *,
                           std::filesystem::path const &) -> void;
  auto module_at(lua_Integer const) noexcept -> Module &;

  // returns -1 on failure
  auto contains(std::filesystem::path const &) const noexcept -> int;

#ifdef DEBUG
  auto dump_paths(std::ostream &) const noexcept -> void;
  auto dump_modules(std::ostream &) const noexcept -> void;
#endif // DEBUG

private:
  auto resize() -> void;

  // maybe switch to these being ints, it's not "correct" to do, but it would
  // make things faster
  size_t cap;
  size_t size;
  std::unique_ptr<bool[]> is_compileds;
  std::unique_ptr<std::vector<std::string>[]> compiled_files; // ?
  std::unique_ptr<std::filesystem::path[]> luamake_paths;
  std::unique_ptr<Module[]> mods;
};
extern LakeModules mods;
} // namespace builtins
} // namespace luamake

#endif
