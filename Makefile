.PHONY: all, ncolor

all:
	c++ -std=c++20 -Wall -Wpedantic -Wconversion -Wpadded -O3 -o luamake_c main.cpp -llua

ncolor:
	c++ -std=c++20 -Wall -Wpedantic -Wconversion -Wpadded -O3 -o luamake_c main.cpp -llua -DNO_TERM_COLOR

clean:
	rm luamake_c
