CXX = g++
CXXFLAGS = -std=c++23 -Wall -Wextra -Werror -fno-omit-frame-pointer -g
TSAN_CXXFLAGS = -std=c++23 -Wall -Wextra -Werror -fsanitize=thread -fno-omit-frame-pointer -g
ASAN_UBSAN_CXXFLAGS = -std=c++23 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -g
TEST_CXXFLAGS = -std=c++23 -Wall -Wextra -Werror -g

SRC = src/main.cpp src/kvstore.cpp src/parser.cpp src/dispatcher.cpp src/encoder.cpp src/server.cpp src/wal.cpp src/snapshot.cpp src/persistence_format.cpp
OBJ = $(SRC:.cpp=.o)

TEST_PIPELINE_SRC = tests/test_pipeline.cpp src/kvstore.cpp src/parser.cpp src/dispatcher.cpp src/encoder.cpp src/wal.cpp src/persistence_format.cpp src/snapshot.cpp
TEST_RECOVERY_SRC = tests/test_recovery.cpp src/kvstore.cpp src/dispatcher.cpp src/wal.cpp src/persistence_format.cpp src/snapshot.cpp
TEST_ORDERING_SRC = tests/test_mutation_ordering.cpp src/kvstore.cpp src/dispatcher.cpp src/wal.cpp src/persistence_format.cpp src/snapshot.cpp
TEST_CHECKPOINT_SRC = tests/test_checkpoint.cpp src/kvstore.cpp src/dispatcher.cpp src/wal.cpp src/persistence_format.cpp src/snapshot.cpp
TEST_WAL_RECOVERY_SRC = tests/test_wal_recovery.cpp src/wal.cpp src/persistence_format.cpp
TEST_SNAPSHOT_RECOVERY_SRC = tests/test_snapshot_recovery.cpp src/kvstore.cpp src/snapshot.cpp src/persistence_format.cpp
TEST_PERSISTENCE_FORMAT_SRC = tests/test_persistence_format.cpp src/persistence_format.cpp
TEST_FAILPOINT_SRC = tests/test_persistence_failpoints.cpp src/kvstore.cpp src/dispatcher.cpp src/wal.cpp src/snapshot.cpp src/persistence_format.cpp

all: kvstore

kvstore: $(OBJ)
	$(CXX) $(CXXFLAGS) -o kvstore $(OBJ)

kvstore-tsan: $(SRC)
	$(CXX) $(TSAN_CXXFLAGS) -I include -o kvstore-tsan $(SRC)

test: test_pipeline test_recovery test_mutation_ordering test_checkpoint test_wal_recovery test_snapshot_recovery test_persistence_format test_persistence_failpoints

test_pipeline: $(TEST_PIPELINE_SRC)
	$(CXX) $(TEST_CXXFLAGS) -pthread -I include -o test_pipeline $(TEST_PIPELINE_SRC)

test_recovery: $(TEST_RECOVERY_SRC)
	$(CXX) $(TEST_CXXFLAGS) -pthread -I include -o test_recovery $(TEST_RECOVERY_SRC)

test_mutation_ordering: $(TEST_ORDERING_SRC)
	$(CXX) $(TEST_CXXFLAGS) -pthread -I include -o test_mutation_ordering $(TEST_ORDERING_SRC)

test_mutation_ordering-tsan: $(TEST_ORDERING_SRC)
	$(CXX) $(TSAN_CXXFLAGS) -pthread -I include -o test_mutation_ordering-tsan $(TEST_ORDERING_SRC)

test_checkpoint: $(TEST_CHECKPOINT_SRC)
	$(CXX) $(TEST_CXXFLAGS) -pthread -I include -o test_checkpoint $(TEST_CHECKPOINT_SRC)

test_checkpoint-tsan: $(TEST_CHECKPOINT_SRC)
	$(CXX) $(TSAN_CXXFLAGS) -pthread -I include -o test_checkpoint-tsan $(TEST_CHECKPOINT_SRC)

test_wal_recovery: $(TEST_WAL_RECOVERY_SRC)
	$(CXX) $(TEST_CXXFLAGS) -I include -o test_wal_recovery $(TEST_WAL_RECOVERY_SRC)

test_snapshot_recovery: $(TEST_SNAPSHOT_RECOVERY_SRC)
	$(CXX) $(TEST_CXXFLAGS) -I include -o test_snapshot_recovery $(TEST_SNAPSHOT_RECOVERY_SRC)

test_persistence_format: $(TEST_PERSISTENCE_FORMAT_SRC)
	$(CXX) $(TEST_CXXFLAGS) -I include -o test_persistence_format $(TEST_PERSISTENCE_FORMAT_SRC)

test_persistence_failpoints: $(TEST_FAILPOINT_SRC)
	$(CXX) $(TEST_CXXFLAGS) -DKVSTORE_ENABLE_FAILPOINTS -pthread -I include -o test_persistence_failpoints $(TEST_FAILPOINT_SRC)

test_wal_recovery-sanitize: $(TEST_WAL_RECOVERY_SRC)
	$(CXX) $(ASAN_UBSAN_CXXFLAGS) -I include -o test_wal_recovery-sanitize $(TEST_WAL_RECOVERY_SRC)

test_snapshot_recovery-sanitize: $(TEST_SNAPSHOT_RECOVERY_SRC)
	$(CXX) $(ASAN_UBSAN_CXXFLAGS) -I include -o test_snapshot_recovery-sanitize $(TEST_SNAPSHOT_RECOVERY_SRC)

test_persistence_format-sanitize: $(TEST_PERSISTENCE_FORMAT_SRC)
	$(CXX) $(ASAN_UBSAN_CXXFLAGS) -I include -o test_persistence_format-sanitize $(TEST_PERSISTENCE_FORMAT_SRC)

sanitize: test_wal_recovery-sanitize test_snapshot_recovery-sanitize test_persistence_format-sanitize test_persistence_failpoints
	./test_wal_recovery-sanitize
	./test_snapshot_recovery-sanitize
	./test_persistence_format-sanitize

stress: test_mutation_ordering test_checkpoint
	./test_mutation_ordering 100
	./test_checkpoint

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -I include -c $< -o $@

clean:
	rm -f $(OBJ) kvstore kvstore-tsan test_pipeline test_recovery test_mutation_ordering test_mutation_ordering-tsan test_checkpoint test_checkpoint-tsan test_wal_recovery test_snapshot_recovery test_persistence_format test_wal_recovery-sanitize test_snapshot_recovery-sanitize test_persistence_format-sanitize
