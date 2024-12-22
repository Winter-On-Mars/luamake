# add -DNO_TERM_COLOR to the c++ call, in order to remove terminal color from luamake

all:
	c++ -std=c++20 -Wall -Wpedantic -Wconversion -Wpadded -O3 -o luamake_c main.cpp -llua
	cc -shared -Wall -Wconversion -O3 -o luamake.so luamake.c -fPIC

lm:
	c++ -std=c++20 -Wall -Wpedantic -Wconversion -Wpadded -O3 -o luamake_c main.cpp -llua

luamake:
	cc -shared -Wall -Wconversion -O3 -o luamake.so luamake.c -fPIC

clean:
	rm luamake_c
