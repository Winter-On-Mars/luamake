cc:=clang++
cc_flags:=-std=c++20 -Wall -Wpedantic -Wconversion -Wpadded -O3
bin_name:=luamake_c# TODO: change this when the c rewrite is done
linker:=mold

.PHONY: all, ncolor

all:
	$(cc) $(cc_flags) -o common.o -c common.cpp
	$(cc) $(cc_flags) -o luamake_builtins.o -c luamake_builtins.cpp
	$(cc) $(cc_flags) -o main.o -c main.cpp
	$(cc) $(cc_flags) -o $(bin_name) main.o common.o -fuse-ld=$(linker) -llua

ncolor:
	$(cc) $(cc_flags) -o common.o -c common.cpp -DNO_TERM_COLOR
	$(cc) $(cc_flags) -o luamake_builtins.o -c luamake_builtins.cpp -DNO_TERM_COLOR
	$(cc) $(cc_flags) -o main.o -c main.cpp -DNO_TERM_COLOR
	$(cc) $(cc_flags) -o $(bin_name) main.o common.o luamake_builtins.o -fuse-ld=$(linker) -llua

clean:
	rm $(bin_name)
