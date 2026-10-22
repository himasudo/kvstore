CXX ?= g++

# `make` builds an optimized server. Pass STRICT=-Werror to make warnings fatal
# there too (CI does). Test and sanitizer builds always use -Werror.
STRICT ?=

COMMON   = -std=c++23 -Wall -Wextra -pthread -fno-omit-frame-pointer -g -I include
FLAGS_rel       = $(COMMON) -O2 $(STRICT)
FLAGS_test      = $(COMMON) -O1 -Werror
FLAGS_fp        = $(FLAGS_test) -DKVSTORE_ENABLE_FAILPOINTS
FLAGS_iof       = $(FLAGS_test) -DKVSTORE_ENABLE_IO_FAULTS
FLAGS_asan      = $(COMMON) -O1 -Werror -fsanitize=address,undefined
FLAGS_asan_fp   = $(FLAGS_asan) -DKVSTORE_ENABLE_FAILPOINTS
FLAGS_asan_iof  = $(FLAGS_asan) -DKVSTORE_ENABLE_IO_FAULTS
FLAGS_tsan      = $(COMMON) -O1 -Werror -fsanitize=thread
FLAGS_netfault  = $(FLAGS_rel) -DKVSTORE_ENABLE_NET_FAULTS

# Objects live in build/<variant>/, so a change in flags or defines never
# reuses an object built for another variant. -MMD -MP tracks header changes.
objs = $(patsubst %.cpp,build/$(1)/%.o,$(2))

define VARIANT
build/$(1)/%.o: %.cpp
	@mkdir -p $$(dir $$@)
	$$(CXX) $$(FLAGS_$(1)) -MMD -MP -c $$< -o $$@
endef
$(foreach v,rel test fp iof asan asan_fp asan_iof tsan netfault,$(eval $(call VARIANT,$(v))))

-include $(wildcard build/*/src/*.d build/*/tests/*.d)

SRC = src/main.cpp src/kvstore.cpp src/parser.cpp src/dispatcher.cpp src/encoder.cpp src/server.cpp src/wal.cpp src/snapshot.cpp src/persistence_format.cpp

TEST_PIPELINE_SRC = tests/test_pipeline.cpp src/kvstore.cpp src/parser.cpp src/dispatcher.cpp src/encoder.cpp src/wal.cpp src/persistence_format.cpp src/snapshot.cpp
TEST_RECOVERY_SRC = tests/test_recovery.cpp src/kvstore.cpp src/dispatcher.cpp src/wal.cpp src/persistence_format.cpp src/snapshot.cpp
TEST_ORDERING_SRC = tests/test_mutation_ordering.cpp src/kvstore.cpp src/dispatcher.cpp src/wal.cpp src/persistence_format.cpp src/snapshot.cpp
TEST_CHECKPOINT_SRC = tests/test_checkpoint.cpp src/kvstore.cpp src/dispatcher.cpp src/wal.cpp src/persistence_format.cpp src/snapshot.cpp
TEST_WAL_RECOVERY_SRC = tests/test_wal_recovery.cpp src/wal.cpp src/persistence_format.cpp
TEST_SNAPSHOT_RECOVERY_SRC = tests/test_snapshot_recovery.cpp src/kvstore.cpp src/snapshot.cpp src/persistence_format.cpp
TEST_PERSISTENCE_FORMAT_SRC = tests/test_persistence_format.cpp src/persistence_format.cpp
TEST_FAILPOINT_SRC = tests/test_persistence_failpoints.cpp src/kvstore.cpp src/dispatcher.cpp src/wal.cpp src/snapshot.cpp src/persistence_format.cpp
TEST_PARSER_SRC = tests/test_parser_incremental.cpp src/parser.cpp
TEST_IO_FAULT_SRC = tests/test_io_faults.cpp src/kvstore.cpp src/dispatcher.cpp src/wal.cpp src/snapshot.cpp src/persistence_format.cpp

# name:source-variable pairs for every unit test
UNIT_TESTS = test_pipeline test_recovery test_mutation_ordering test_checkpoint \
             test_wal_recovery test_snapshot_recovery test_persistence_format \
             test_persistence_failpoints test_io_faults test_parser_incremental

# $(1)=binary name  $(2)=source list variable  $(3)=variant
define LINK
$(1): $$(call objs,$(3),$$($(2)))
	$$(CXX) $$(FLAGS_$(3)) -o $$@ $$^
endef

$(eval $(call LINK,kvstore,SRC,rel))
$(eval $(call LINK,kvstore-tsan,SRC,tsan))
$(eval $(call LINK,kvstore-network-sanitize,SRC,asan))
$(eval $(call LINK,kvstore-netfault,SRC,netfault))

$(eval $(call LINK,test_pipeline,TEST_PIPELINE_SRC,test))
$(eval $(call LINK,test_recovery,TEST_RECOVERY_SRC,test))
$(eval $(call LINK,test_mutation_ordering,TEST_ORDERING_SRC,test))
$(eval $(call LINK,test_checkpoint,TEST_CHECKPOINT_SRC,test))
$(eval $(call LINK,test_wal_recovery,TEST_WAL_RECOVERY_SRC,test))
$(eval $(call LINK,test_snapshot_recovery,TEST_SNAPSHOT_RECOVERY_SRC,test))
$(eval $(call LINK,test_persistence_format,TEST_PERSISTENCE_FORMAT_SRC,test))
$(eval $(call LINK,test_persistence_failpoints,TEST_FAILPOINT_SRC,fp))
$(eval $(call LINK,test_io_faults,TEST_IO_FAULT_SRC,iof))
$(eval $(call LINK,test_parser_incremental,TEST_PARSER_SRC,test))

$(eval $(call LINK,test_mutation_ordering-tsan,TEST_ORDERING_SRC,tsan))
$(eval $(call LINK,test_checkpoint-tsan,TEST_CHECKPOINT_SRC,tsan))

$(eval $(call LINK,test_wal_recovery-sanitize,TEST_WAL_RECOVERY_SRC,asan))
$(eval $(call LINK,test_snapshot_recovery-sanitize,TEST_SNAPSHOT_RECOVERY_SRC,asan))
$(eval $(call LINK,test_persistence_format-sanitize,TEST_PERSISTENCE_FORMAT_SRC,asan))
$(eval $(call LINK,test_parser_incremental-sanitize,TEST_PARSER_SRC,asan))
$(eval $(call LINK,test_io_faults-sanitize,TEST_IO_FAULT_SRC,asan_iof))
$(eval $(call LINK,test_persistence_failpoints-sanitize,TEST_FAILPOINT_SRC,asan_fp))

SANITIZED_TESTS = test_wal_recovery-sanitize test_snapshot_recovery-sanitize \
                  test_persistence_format-sanitize test_parser_incremental-sanitize \
                  test_io_faults-sanitize test_persistence_failpoints-sanitize

PY_TESTS = test_tcp_protocol test_network_stress test_startup_failure test_crash_recovery \
           test_torn_tail_startup test_startup_errors

all: kvstore

# Build every unit test binary without running it.
test: $(UNIT_TESTS)

# Build and run every unit test; stops at the first failure.
check-unit: test
	@set -e; for t in $(UNIT_TESTS); do echo "== $$t"; ./$$t; done

# Server integration tests (real TCP, crash/restart, injected network faults).
check-integration: kvstore kvstore-netfault
	@set -e; for t in $(PY_TESTS); do echo "== $$t"; python3 tests/$$t.py ./kvstore; done
	@echo "== test_network_faults"; python3 tests/test_network_faults.py ./kvstore-netfault

# Run the stages one after another even under `make -j`: the integration
# tests share TCP port 6379 and the stress tests are timing sensitive.
check:
	$(MAKE) check-unit
	$(MAKE) check-integration
	$(MAKE) stress

sanitize: $(SANITIZED_TESTS)
	@set -e; for t in $(SANITIZED_TESTS); do echo "== $$t"; ./$$t; done

stress: test_mutation_ordering test_checkpoint
	./test_mutation_ordering 100
	./test_checkpoint

clean:
	rm -rf build
	rm -f kvstore kvstore-tsan kvstore-netfault kvstore-network-sanitize \
	      $(UNIT_TESTS) test_mutation_ordering-tsan test_checkpoint-tsan $(SANITIZED_TESTS)

.PHONY: all test check check-unit check-integration sanitize stress clean
