cc:=clang++
cc_flags:=-std=c++20 -Wall -Wpedantic -Wconversion -Wpadded -O3
bin_name:=luamake_c# TODO: change this when the c rewrite is done
linker:=mold

.PHONY: all, ncolor, dbg, clean_submod

files:=common.o luamake_builtins.o dependency_graph.o main.o
lua_a:=lua/liblua.a

all: $(files) $(lua_a)
	$(cc) $(cc_flags) -o $(bin_name) $(files) -fuse-ld=$(linker) $(lua_a)

dbg: cc_flags+=-g
dbg: $(files)
	$(cc) $(cc_flags) -o $(bin_name) $(files) -fuse-ld=$(linker)

ncolor: cc_flags+=-DNO_TERM_COLOR
ncolor: $(files)
	$(cc) $(cc_flags) -o $(bin_name) $(files) -fuse-ld=$(linker)

%.o: %.cpp
	$(cc) $(cc_flags) -o $@ -c $^

$(lua_a):
	$(MAKE) -C lua a -j4

clean:
	rm $(bin_name) *.o

clean_submod:
	rm $(lua_a) lua/*.o
