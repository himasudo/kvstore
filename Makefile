CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Werror -g

SRC = src/main.cpp src/kvstore.cpp
OBJ = $(SRC:.cpp=.o)

all: kvstore

kvstore: $(OBJ)
	$(CXX) $(CXXFLAGS) -o kvstore $(OBJ)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -I include -c $< -o $@

clean:
	rm -f $(OBJ)