# things that are allowed to be changed with some defaults
LAKE_BUILD_DIR:=build
LAKE_LINKER:=mold
LAKE_BIN:=luamake_c#TODO: change this when the c rewrite is done
LAKE_CC:=clang++
# things that can be changed but probably shouldn't
_cc_flags:=-std=c++20 -Wall -Wpedantic -Wconversion -Wpadded -fno-rtti $(LAKE_CCFLAGS) -iquote lua/build
_bin_name:=$(LAKE_BUILD_DIR)/$(LAKE_BIN)
_includes:=lua

.PHONY: all, dbg, ncolor, release, clean_submod, perf_testing

_files:=$(LAKE_BUILD_DIR)/common.o $(LAKE_BUILD_DIR)/luamake_strings.o \
				$(LAKE_BUILD_DIR)/luamake_pre_ir.o $(LAKE_BUILD_DIR)/luamake_file.o \
				$(LAKE_BUILD_DIR)/luamake_builtins.o $(LAKE_BUILD_DIR)/luamake_string_manip.o \
				$(LAKE_BUILD_DIR)/luamake_thread_pool.o $(LAKE_BUILD_DIR)/main.o \
				$(LAKE_BUILD_DIR)/luamake_spiral.o $(LAKE_BUILD_DIR)/luamake_allocator.o
_lua_a:=lua/liblua.a

all: $(_files) $(_lua_a)
	$(CC) $(_cc_flags) -o $(_bin_name) $(_files) -fuse-ld=$(LAKE_LINKER) $(_lua_a)

dbg: _cc_flags+=-ggdb3 -DDEBUG -fno-omit-frame-pointer
dbg: $(_files) $(_lua_a)
	$(LAKE_CC) $(_cc_flags) -o $(_bin_name) $(_files) -fuse-ld=$(LAKE_LINKER) --for-linker=--gdb-index  $(_lua_a)

ncolor: _cc_flags+=-DNO_TERM_COLOR
ncolor: $(_files) $(_lua_a)
	$(LAKE_CC) $(_cc_flags) -o $(_bin_name) $(_files) -fuse-ld=$(LAKE_LINKER) $(_lua_a)

release: _cc_flags+=-O3 -ffast-math -flto -march=native
release: $(_files) $(_lua_a)
	$(LAKE_CC) $(_cc_flags) -o $(_bin_name) $(_files) -fuse-ld=$(LAKE_LINKER) $(_lua_a)

perf_testing: _cc_flags+=-DPERF_TESTING -O3 -ffast-math -flto -march=native -ggdb3
perf_testing: $(_files) $(_lua_a)
	$(LAKE_CC) $(_cc_flags) -o $(_bin_name) $(_files) -fuse-ld=$(LAKE_LINKER) $(_lua_a)

build/%.o: src/%.cpp
	$(LAKE_CC) $(_cc_flags) -o $@ -c $^ -I$(_includes)

$(_lua_a):
	$(MAKE) -C lua a

clean:
	rm $(_bin_name) build/*.o

clean_submod:
	rm $(_lua_a) lua/*.o
