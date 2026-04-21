CXX = g++
CXXFLAGS = -std=c++23 -Wall -Wextra -Werror -fsanitize=thread -fno-omit-frame-pointer -g 

SRC = src/main.cpp src/kvstore.cpp src/parser.cpp src/dispatcher.cpp src/encoder.cpp src/server.cpp
OBJ = $(SRC:.cpp=.o)

TEST_SRC = tests/test_pipeline.cpp src/kvstore.cpp src/parser.cpp src/dispatcher.cpp src/encoder.cpp
TEST_OBJ = $(TEST_SRC:.cpp=.o)

all: kvstore

kvstore: $(OBJ)
	$(CXX) $(CXXFLAGS) -o kvstore $(OBJ)

test: $(TEST_OBJ)
	$(CXX) $(CXXFLAGS) -o test_pipeline $(TEST_OBJ)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -I include -c $< -o $@

clean:
	rm -f $(OBJ) $(TEST_OBJ) kvstore test_pipeline