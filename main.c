#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

typedef char * string;

// should port this to require that pattern is a small string
// i.e. that pattern.size() < sizeof(void*)
bool string_contains(string str, string pattern) {
	long unsigned const str_len = strlen(str);
	long unsigned const pat_len = strlen(pattern);
	if (pat_len > str_len) {
		return false;
	}

	char window[pat_len];
	memcpy((void*)window, (void*)str, sizeof(char) * pat_len);

	for (int i = 1; i < str_len - pat_len + 2; ++i) {
		if (strncmp(window, pattern, 2) == 0) {
			return true;
		}
		memcpy((void*)window, (void*)(str + i), sizeof(char) * pat_len);
	}
	return false;
}

typedef struct {
	string compiler;
	string* flags;
	string name;
	string type;
	string* linking;
	string linking_dir;
	string* pre_exec;
	string* post_exec;
} config;

void write_to_config(config* cfg, lua_State* state) { 
	// returns the type of the global value, which we've already checked to be
	// table so we can ignore it
	(void)lua_getglobal(state, "config_table");

	// creating a buffer so 'config_table' is slightly protected on the stack :)
	lua_pushnil(state);

	lua_getglobal(state, "Dump");
	lua_getfield(state, -3, "compiler"); // -3 bc we push 'Dump' to the stack, so config_table now 3 below the top
	lua_call(state, 1,1);
	char const * cfg_str = luaL_tolstring(state, -1, NULL);
	printf("%s\n", cfg_str);
}

// <hash table>

typedef struct {
	string key;
	size_t value;
} entry;

typedef struct {
	int count;
	int capacity;
	entry* entries;
} file_hashes;

void init_hash(file_hashes* hashes) {
	hashes->count = 0;
	hashes->capacity = 0;
	hashes->entries = NULL;
}

void free_hash(file_hashes* hashes) {
	free(hashes->entries);
	init_hash(hashes);
}

// </hash table>

typedef struct {
	int opt_level;
	int clean;
	int debug_info;
} make_flags;

typedef struct {
	config * cfg;
	file_hashes files;
	make_flags flags;
} cache;

make_flags parse_compiler_flags(int argc, char** argv) {
	make_flags flags = {.clean = 0, .opt_level = 0, .debug_info = 0};
	for (int i = 0; i < argc; ++i) {
		if (strcmp(argv[i], "-c") == 0) {
			flags.clean = 1;
		} else if (strcmp(argv[i], "-r") == 0) {
			flags.opt_level = 1;
		} else if (strcmp(argv[i], "-g") == 0) {
			flags.debug_info = 1;
		}
	}
	return flags;
}

// TODO: make this actually clean the dir just like the lua version
void clean_dir(string path) {
	fprintf(stdout, "\tremoving .o, .out, and cache files from %s/build/\n", path);

	DIR* dir;
	struct dirent* ent;
	if ((dir = opendir(path)) != NULL) {
		fprintf(stdout, "opened\n");
		while ((ent = readdir(dir)) != NULL) {
			if (string_contains(ent->d_name, ".o")) {
				fprintf(stdout, "found object file %s\n", ent->d_name);
			}
			fprintf(stdout, "%s\n", ent->d_name);
		}
		closedir(dir);
	} else {
		fprintf(stderr, "Error occured\n");
	}
}

// TODO: check return codes to make sure they line up with user error vs internal service error
int main(int argc, char** argv) {
	if (argc == 1) {
		print_incorrect_usage();
		return 2;
	}

	lua_State* state = luaL_newstate();
	luaL_openlibs(state);
	if (state == NULL) {
		fprintf(stderr, "Unable to init lua state machine\n");
		return 65;
	}

	// turn off gc, might improve perf idk, should probaby test :)
	int const gc_res = lua_gc(state, LUA_GCSTOP);

	// TODO: add a -h flag for help message

	string dir = argv[1];
	// this seems to be working, but no telling with c_strings :)
	if (dir[strlen(dir) - 1] == '/') {
		dir[strlen(dir) - 1] = 0;
	}
	printf("testing dir path: %s\n", dir);

	// load our helper lua function bc i'm new to working with the 
	// lua c api
	if (luaL_dofile(state, "/home/izikp/dev/luamake/helper.lua") != LUA_OK) {
		fprintf(stderr, "Unable to load helper.lua\n");
		return 65;
	}

	// TODO: update with better error message
	if (luaL_dofile(state, "config.lua") != LUA_OK) {
		fprintf(stderr, "Error parsing config.lua\n");
		return 65;
	}

	if (!lua_istable(state, -1)) {
		fprintf(stderr, "Error parsing config.lua, expected table found not a table\nMight be an issue with project layout, see README for details about expected project layout\nIf not please open an issue on the Github with Details.");
		return 67;
	}

	// config table should be at the top of the stack,
	// so we just assign it to a global so we can access to it the whole time
	// probably a better way to go about this but this seems to be working :)
	lua_setglobal(state, "config_table");

	config cfg;
	write_to_config(&cfg, state);
	
	make_flags flags = parse_compiler_flags(argc, argv);

	if (flags.clean == 1) {
		clean_dir(dir);
	} else {
		fprintf(stdout, "Building with config: {\n\tcompiler: %s,\n\tflags: %s,\n\tbinary name: %s,\n\tlinking: %s,\n\tpre_exec: %s,\n\tpost_exec: %s\n}\n",
		"", "", "", "", "", "");
	}
	
	lua_close(state);

	return 0;
}
