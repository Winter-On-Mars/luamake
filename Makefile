# add -DNO_TERM_COLOR to the c++ call, in order to remove terminal color from luamake

all:
	c++ -std=c++20 -Wall -Wpedantic -Wconversion -Wpadded -O3 -o luamake_c main.cpp -llua

clean:
	rm luamake_c
