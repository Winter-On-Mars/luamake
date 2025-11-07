cc:=clang++
cc_flags:=-std=c++20 -Wall -Wpedantic -Wconversion -Wpadded -fno-rtti# -fsanitize=address
bin_name:=build/luamake_c# TODO: change this when the c rewrite is done
linker:=lld # if someone has clang they should have lld, so this is better, even if mold is a better linker
includes:=lua

.PHONY: all, dbg, ncolor, release, clean_submod

files:=build/common.o build/luamake_strings.o build/luamake_pre_ir.o build/luamake_file.o build/luamake_builtins.o build/main.o
lua_a:=lua/liblua.a

all: $(files) $(lua_a)
	$(cc) $(cc_flags) -o $(bin_name) $(files) -fuse-ld=$(linker) $(lua_a)

dbg: cc_flags+=-ggdb3 -DDEBUG# might be a good idea to just use -g, but idk i only use gdb for debugging :)
dbg: $(files) $(lua_a)
	$(cc) $(cc_flags) -o $(bin_name) $(files) -fuse-ld=$(linker) $(lua_a)

ncolor: cc_flags+=-DNO_TERM_COLOR
ncolor: $(files)
	$(cc) $(cc_flags) -o $(bin_name) $(files) -fuse-ld=$(linker) $(lua_a)

release: cc_flags+=-O3 -ffast-math -flto -march=native
release: $(files)
	$(cc) $(cc_flags) -o $(bin_name) $(files) -fuse-ld=$(linker) $(lua_a)

build/%.o: src/%.cpp
	$(cc) $(cc_flags) -o $@ -c $^ -I$(includes)

$(lua_a):
	$(MAKE) -C lua a -j4

clean:
	rm $(bin_name) build/*.o

clean_submod:
	rm $(lua_a) lua/*.o
