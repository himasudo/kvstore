CXX = g++
CXXFLAGS = -std=c++23 -Wall -Wextra -Werror -fno-omit-frame-pointer -g
TSAN_CXXFLAGS = -std=c++23 -Wall -Wextra -Werror -fsanitize=thread -fno-omit-frame-pointer -g
TEST_CXXFLAGS = -std=c++23 -Wall -Wextra -Werror -g

SRC = src/main.cpp src/kvstore.cpp src/parser.cpp src/dispatcher.cpp src/encoder.cpp src/server.cpp src/wal.cpp src/snapshot.cpp
OBJ = $(SRC:.cpp=.o)

TEST_SRC = tests/test_pipeline.cpp src/kvstore.cpp src/parser.cpp src/dispatcher.cpp src/encoder.cpp src/wal.cpp
TEST_OBJ = $(TEST_SRC:.cpp=.o)

all: kvstore

kvstore: $(OBJ)
	$(CXX) $(CXXFLAGS) -o kvstore $(OBJ)

kvstore-tsan: $(SRC)
	$(CXX) $(TSAN_CXXFLAGS) -I include -o kvstore-tsan $(SRC)

test:
	$(CXX) $(TEST_CXXFLAGS) -I include -o test_pipeline $(TEST_SRC)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -I include -c $< -o $@

clean:
	rm -f $(OBJ) $(TEST_OBJ) kvstore test_pipeline