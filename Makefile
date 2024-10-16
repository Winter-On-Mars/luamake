all:
	c++ -std=c++20 -Wall -Wpedantic -Wconversion -Wpadded -O3 -o luamake_c main.cpp -llua

clean:
	rm luamake_c
