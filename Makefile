cc:=clang++
cc_flags:=-std=c++20 -Wall -Wpedantic -Wconversion -Wpadded -O3
bin_name:=luamake_c# TODO: change this when the c rewrite is done
linker:=mold

.PHONY: all, ncolor

files:=common.o luamake_builtins.o dependency_graph.o main.o

all: $(files)
	$(cc) $(cc_flags) -o $(bin_name) $(files) -fuse-ld=$(linker) -llua

ncolor: cc_flags += -DNO_TERM_COLOR
ncolor: $(files)
	$(cc) $(cc_flags) -o $(bin_name) $(files) -fuse-ld=$(linker) -llua

%.o: %.cpp
	$(cc) $(cc_flags) -o $@ -c $^

clean:
	rm $(bin_name) *.o
