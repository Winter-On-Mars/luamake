cc:=clang++
cc_flags:=-std=c++20 -Wall -Wpedantic -Wconversion -Wpadded -O3
bin_name:=luamake_c# change this when the c rewrite is done

.PHONY: all, ncolor

all:
	$(cc) $(cc_flags) -o $(bin_name) main.cpp -llua

ncolor:
	$(cc) $(cc_flags) -o $(bin_name) main.cpp -llua -DNO_TERM_COLOR

clean:
	rm $(bin_name)
