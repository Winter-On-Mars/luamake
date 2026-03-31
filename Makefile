cc:=clang++
cc_flags:=-std=c++20 -Wall -Wpedantic -Wconversion -Wpadded -fno-rtti
bin_name:=build/luamake_c# TODO: change this when the c rewrite is done
linker:=lld # if someone has clang they should have lld, so this is better, even if mold is a better linker
includes:=lua

.PHONY: all, dbg, ncolor, release, clean_submod, perf_testing, perf_testing_release

files:=build/common.o build/luamake_strings.o build/luamake_pre_ir.o build/luamake_file.o build/luamake_builtins.o build/luamake_string_manip.o build/luamake_thread_pool.o build/main.o
lua_a:=lua/liblua.a

all: $(files) $(lua_a)
	$(cc) $(cc_flags) -o $(bin_name) $(files) -fuse-ld=$(linker) $(lua_a)

# it seems like directly using fork is causing issues with fsanitize=address(?), if somebody is able to debug the issue and make a change that would be nice
# that or there is still a memory leak and i'm lying to myself, idk valgrind seems to just vomit whenever i run it, i assume partially because lua has a gc in it, valgrind reports that a lot of things in the lua vm are potentially lost
dbg: cc_flags+=-ggdb3 -DDEBUG -fno-omit-frame-pointer #-DPERF_TESTING
dbg: $(files) $(lua_a)
	$(cc) $(cc_flags) -o $(bin_name) $(files) -fuse-ld=$(linker) $(lua_a)

ncolor: cc_flags+=-DNO_TERM_COLOR
ncolor: $(files)
	$(cc) $(cc_flags) -o $(bin_name) $(files) -fuse-ld=$(linker) $(lua_a)

release: cc_flags+=-O3 -ffast-math -flto -march=native
release: $(files)
	$(cc) $(cc_flags) -o $(bin_name) $(files) -fuse-ld=$(linker) $(lua_a)

perf_testing: cc_flags+=-ggdb3 -DPERF_TESTING
perf_testing: $(files) $(lua_a)
	$(cc) $(cc_flags) -o $(bin_name) $(files) -fuse-ld=$(linker) $(lua_a)

perf_testing_release: cc_flags+=-DPERF_TESTING -O3 -ffast-math -flto -march=native
perf_testing_release: $(files) $(lua_a)
	$(cc) $(cc_flags) -o $(bin_name) $(files) -fuse-ld=$(linker) $(lua_a)

build/%.o: src/%.cpp
	$(cc) $(cc_flags) -o $@ -c $^ -I$(includes)

$(lua_a):
	$(MAKE) -C lua a -j4

clean:
	rm $(bin_name) build/*.o

clean_submod:
	rm $(lua_a) lua/*.o
