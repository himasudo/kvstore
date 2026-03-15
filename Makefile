CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Werror -g

all:
	$(CXX) $(CXXFLAGS) src/main.cpp -o kvstore

clean:
	rm -f kvstore