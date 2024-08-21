all:
	c++ -Wall -Wpedantic -Wconversion -O3 -o luamake_c main.cpp -llua

clean:
	rm luamake_c
