cc:=clang++
cc_flags:=-std=c++20 -Wall -Wpedantic -Wconversion -Wpadded -fno-rtti # -fsanitize=address
bin_name:=luamake_c# TODO: change this when the c rewrite is done
linker:=lld # if someone has clang they should have lld, so this is better, even if mold is a better linker

.PHONY: all, ncolor, dbg, clean_submod

files:=common.o luamake_builtins.o dependency_graph.o main.o luamake_error.o luamake_strings.o luamake_pre_ir.o
lua_a:=lua/liblua.a

all: $(files) $(lua_a)
	$(cc) $(cc_flags) -o $(bin_name) $(files) -fuse-ld=$(linker) $(lua_a)

dbg: cc_flags+=-ggdb3 -DDEBUG # might be a good idea to just use -g, but idk i only use gdb for debugging :)
dbg: $(files) $(lua_a)
	$(cc) $(cc_flags) -o $(bin_name) $(files) -fuse-ld=$(linker) $(lua_a)

ncolor: cc_flags+=-DNO_TERM_COLOR
ncolor: $(files)
	$(cc) $(cc_flags) -o $(bin_name) $(files) -fuse-ld=$(linker) $(lua_a)

%.o: %.cpp
	$(cc) $(cc_flags) -o $@ -c $^

$(lua_a):
	$(MAKE) -C lua a -j4

clean:
	rm $(bin_name) *.o

clean_submod:
	rm $(lua_a) lua/*.o
