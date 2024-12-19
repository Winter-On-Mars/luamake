#include <algorithm>
#include <cstring>
#include <iostream>
#include <numeric>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <filesystem>

#include <lua.hpp>

using std::vector, std::string, std::string_view;

namespace helper {
auto constexpr flatten(std::span<string_view const> const arr,
                       string_view const seperator) noexcept -> string {
  auto const sep = string(seperator);
  return std::accumulate(
    arr.begin(), arr.end(), string(),
    [sep](auto&& init, auto&& n) { return (init + sep) + string(n); }
  );
}

// how tf do you do cpp doc comments >:(
/* @description: dumps content of lua value in standard way */
auto constexpr dump(lua_State* state /*, lua_table* ...*/) noexcept -> string {
  // TODO: actually write function
  return string("");
}
} // namespace helper

// all of the strings we parse from the config.lua are owned by
// the lua_State gc, so these all are non-owning references
struct Config final {
  string_view compiler;
  vector<string_view> flags;
  string_view name;
  string_view type;
  vector<string_view> linking;
  string_view linking_dir;
  vector<string_view> pre_exec;
  vector<string_view> post_exec;

  static auto make(lua_State *) noexcept -> std::optional<Config>;

  friend auto operator<<(std::ostream &,
                         Config const &) noexcept -> std::ostream &;
};

// See what functions modify the lua_State stack
auto Config::make(lua_State *state) noexcept -> std::optional<Config> {
  // returns the type of the global as an int
  // all of the lua_get... functions seem to just return the type of the
  // returned value (could be useful)
  (void)lua_getglobal(state, "config_table");
  int cfg_tbl_pos_in_stack = -1;

  (void)lua_getfield(state, cfg_tbl_pos_in_stack, "compiler");
  --cfg_tbl_pos_in_stack;
  auto *compiler_str = luaL_tolstring(state, -1, NULL);
  // luaL_tolstring pushes a value onto the stack for some reason :(
  --cfg_tbl_pos_in_stack;

  (void)lua_getfield(state, cfg_tbl_pos_in_stack, "bin_name");
  --cfg_tbl_pos_in_stack;
  auto *bin_name_str = luaL_tolstring(state, -1, NULL);
  --cfg_tbl_pos_in_stack;

  // checking to see what happens if we try to get a field that doesn't exist
  (void)lua_getfield(state, cfg_tbl_pos_in_stack, "output_type");
  --cfg_tbl_pos_in_stack;
  // pretty sure we could just use lua_isnil(state, -1)
  auto output_type =
      (lua_type(state, -1) == LUA_TNIL)
          ? (std::cout
                 << "No type found: Defaulting to binary output (If this isn't "
                    "what you want read the README for more information)\n",
             std::string_view{"binary"})
          : (--cfg_tbl_pos_in_stack,
             std::string_view{luaL_tolstring(state, -1, NULL)});

  auto flags = vector<string_view>();
  flags.reserve(4);
  (void)lua_getfield(state, cfg_tbl_pos_in_stack, "flags");
  if (lua_isnil(state, -1)) {
    std::cout << "Error: config.flags == nil\n";
    std::exit(64);
  }
  if (!lua_istable(state, -1)) {
    std::cout << "Error: typeof(config.flags) ~= table\n";
    std::exit(64);
  }
  int flags_pos_in_stack = -1;
  --cfg_tbl_pos_in_stack;

  auto const len = lua_rawlen(state, flags_pos_in_stack);
  for (auto i = 1ull; i <= len; ++i) {
    (void)lua_rawgeti(state, flags_pos_in_stack, static_cast<lua_Integer>(i));
    --flags_pos_in_stack;
    --cfg_tbl_pos_in_stack;
    auto tmp = string_view{luaL_tolstring(state, -1, NULL)};
    flags.push_back(tmp);
  }

  return Config{/* compiler= */ string_view{compiler_str},
                /* flags= */ flags,
                /* name (out_name)= */ string_view{bin_name_str},
                /* type= */ output_type,
                /* linking= */ vector<string_view>(),
                /* linking_dir= */ string_view{},
                /* pre_exec= */ vector<string_view>(),
                /* post_exec= */ vector<string_view>()};
}

auto operator<<(std::ostream &os,
                Config const &cfg) noexcept -> std::ostream & {
  // don't like that we have to cast the std::string_view to a std::string,
  // because it seems like there should be string::operator+(string_view)
  // but whatever :)
  auto const flags =
      std::accumulate(cfg.flags.cbegin(), cfg.flags.cend(), std::string(),
                      [](auto &&init, auto &&next) {
                        return (init + ", ") + std::string(next);
                      });
  auto const linking =
      cfg.linking.size() == 0
          ? std::string("nil")
          : std::accumulate(cfg.linking.cbegin(), cfg.linking.cend(),
                            std::string(), [](auto &&init, auto &&next) {
                              return init + ", " + std::string(next);
                            });

  auto const pre_exec =
      cfg.pre_exec.size() == 0
          ? std::string("nil")
          : std::accumulate(cfg.pre_exec.cbegin(), cfg.pre_exec.cend(),
                            std::string(), [](auto &&init, auto &&next) {
                              return init + ", " + std::string(next);
                            });
  auto const post_exec =
      cfg.post_exec.size() == 0
          ? std::string("nil")
          : std::accumulate(cfg.post_exec.cbegin(), cfg.post_exec.cend(),
                            std::string(), [](auto &&init, auto &&next) {
                              return init + ", " + std::string(next);
                            });

  os << "{\n\tcompiler: " << cfg.compiler;
  os << ",\n\tflags: " << flags;
  os << ",\n\tbin_name: " << cfg.name;
  os << ",\n\ttype: " << cfg.type;
  os << ",\n\tlinking: " << linking;
  os << ",\n\tlinking_dir: " << cfg.linking_dir;
  os << ",\n\tpre_exec: " << pre_exec;
  os << ",\n\tpost_exec: " << post_exec;

  os << "\n}\n";

  return os;
}

// TODO: try to rewrite this to be more memory efficient, maybe with a memory allocator or something like that
struct src_file final {
  string name;
  std::size_t hash;
  bool recompile;
  vector<src_file> deps;

  auto constexpr notify_deps() noexcept -> void;

  auto constexpr depens_on(string_view const src) const noexcept -> bool {
    if (name == src) {
      return true;
    }
    if (std::any_of(deps.begin(), deps.end(), [src](auto&& dep) { return dep.name == src; })) {
      return true;
    }
    return false;
  }
};

struct MakeFlags final {
  enum class opt_level : int8_t {
    O0,
    O1,
    O2,
    O3,
  };

  std::string_view build_dir = std::string_view(".");

  opt_level level = opt_level::O0;
  bool clean = false;
  bool debug_info = false;

  // try and move this to be std::span<std::string_view> maybe by part 2 rewrite
  // :)
  static auto make(std::span<char *> const) noexcept -> MakeFlags;
};

auto MakeFlags::make(std::span<char *> const args) noexcept -> MakeFlags {
  auto flags = MakeFlags{};
  for (auto const flag : args) {
    if (std::strcmp(flag, "-c") == 0) {
      flags.clean = true;
    } else if (std::strcmp(flag, "-r") == 0) {
      flags.level = opt_level::O3;
    } else if (std::strcmp(flag, "-g") == 0) {
      flags.debug_info = true;
    } else {
      if (std::filesystem::is_directory(flag)) {
        flags.build_dir = std::string_view(flag, strlen(flag));
      } else {
        if (flag != args[0]) {
          std::cerr << "Error parsing command line args\nNo idea what [" << flag
                    << "] is, so it's being ignored. See README for more info.";
        }
      }
    }
  }
  return flags;
}

auto constexpr get_all_files(MakeFlags const& flags) noexcept -> std::pair<std::unordered_map<string, src_file>, cache>{
  auto files = std::unordered_map<string, src_file>();
  for (auto const& files : std::filesystem::open_dir(flags.build_dir)) {
    std::cout << files.name << '\n';
  }

  if (!has_cache_dir(flags.build_dir)) {
  }
}

auto make_dir(Config const &cfg, MakeFlags const &flags) noexcept -> void {
  // if cfg.pre_exec.empty() then loop shouldn't run
  for (auto const command : cfg.pre_exec) {
    std::cout << '\t' << command << '\n';
    // TODO: figure out how to run commands
    // think we might have to make some function
    // to allow for this to work across platforms
  }

  auto const [cpp_files, cache] = get_all_files(flags);
  auto const same_flags = flags == cache.flags;
  cache.flags = flags;

  if (!same_flags) {
    // for loop
  }

  // TODO: see about adding the debug info flag to this, if that results in any
  // speed improvements or things like that
  auto const config_flags =
      cfg.flags.empty() ? string("")
                        : string(" -") + helper::flatten(cfg.flags, " -");
}

auto inline print_incorrect_usage() noexcept -> void {
  std::cerr << "Usage: input the subdir being compiled\n";
}

auto clean_dir(std::string_view const dir) noexcept -> void {}

auto main(int argc, char **argv) -> int {
  // TODO: add help message and -h flag

  auto const cl_args = std::span<char *>(argv, static_cast<size_t>(argc));

  auto *state = luaL_newstate();
  if (state == nullptr) {
    std::cerr << "Unable to init lua vm\n";
    return 65;
  }
  // luaL_openlibs(state);
  // have to turn off the gc, bc we need the strings to stay alive for the whole
  // program idk if we should maybe have the Config object own the strings but
  // this seems fine for a first draft :)
  auto const _ = lua_gc(state, LUA_GCSTOP);

  if (luaL_dofile(state, "config.lua") != LUA_OK) {
    std::cerr << "Error parsing config.lua\n";
    return 67;
  }

  // now config should be at the top of the stack
  if (!lua_istable(state, -1)) {
    std::cerr << "Error parsing config.lua, expected table found not a "
                 "table\nMight be an issue with project layout, see README for "
                 "details about expected project layout\nIf not please open an "
                 "issue on the Github with Details.";
    return 67;
  }

  lua_setglobal(state, "config_table");

  // crash if we're unable to parse it :)
  auto const config = [state]() -> Config {
    auto cfg = Config::make(state);
    if (cfg.has_value()) {
      return *cfg;
    } else {
      std::cerr << "Error while parsing config.lua, todo fix issues";
      std::exit(65);
    }
  }();

  std::cout << config << '\n';

  auto const flags = MakeFlags::make(cl_args);

  if (flags.clean) {
    clean_dir(flags.build_dir);
  } else {
    make_dir(config, flags);
  }

  return 0;
}
