#include <iostream>
#include <string_view>
#include <optional>
#include <vector>

#include <lua.hpp>

// all of the strings we parse from the config.lua are owned by
// the lua_State gc, so these all are non-owning references
struct Config final {
	std::string_view compiler;
	std::vector<std::string_view> flags;
	std::string_view name;
	std::string_view type;
	std::vector<std::string_view> linking;
	std::string_view linking_dir;
	std::vector<std::string_view> pre_exec;
	std::vector<std::string_view> post_exec;

	static auto make(lua_State* state) noexcept -> std::optional<Config>;
};

auto static print_incorrect_usage() noexcept -> void {
	std::cerr << "Usage: input the subdir being compiled\n";
}

auto main(int argc, char** argv) -> int {
	if (argc == 1) {
		print_incorrect_usage();
		// check error code
		return 2;
	}

	auto* state = luaL_newstate();
	if (state == nullptr) {
		std::cerr << "Unable to ini lua vm\n";
		return 65;
	}
	luaL_openlibs(state);
	// have to turn off the gc, bc we need the strings to stay alive for the whole program
	// idk if we should maybe have the Config object own the strings but this seems fine
	// for a first draft :)
	auto const _ = lua_gc(state, LUA_GCSTOP);

	// change this so that we assume that we're building the current dir
	// and use the config.lua files as something like project configs, like
	// cargo.toml files in rust
	if (luaL_dofile(state, "/home/izikp/dev/luamake/helper.lua") != LUA_OK) {
		std::cerr << "Unable to load helper.lua\n";
		return 65;
	}

	if (luaL_dofile(state, "config.lua") != LUA_OK) {
		std::cerr << "Error parsing config.lua\n";
		return 67;
	}

	// crash if we're unable to parse it :)
	auto const config = Config::make(state).value();

	auto const flags = MakeFlags::make(argc, argv);

	std::cout << "Hello World!\n";
	return 0;
}
