CXX ?= c++
OPENSSL_PREFIX ?= $(shell if command -v brew >/dev/null 2>&1; then brew --prefix openssl@3 2>/dev/null; fi)
CPPFLAGS += -Ithird_party $(if $(OPENSSL_PREFIX),-I$(OPENSSL_PREFIX)/include)
CXXFLAGS ?= -O2 -g -std=c++17 -Wall -Wextra -Wpedantic
ifneq ($(strip $(OPENSSL_PREFIX)),)
LDFLAGS += -L$(OPENSSL_PREFIX)/lib -Wl,-rpath,$(OPENSSL_PREFIX)/lib
endif
LDLIBS += -lcrypto -lz -pthread
BIN = build/bin/arbor_node
SAGUARO_BIN = build/bin/saguaro_node
SHARPER_BIN = build/bin/sharper_node
AHL_BIN = build/bin/ahl_node
NETWORK_TEST = build/bin/test_network
DIGEST_TEST = build/bin/test_state_digest
SHARPER_RECOVERY_TEST = build/bin/test_sharper_recovery
ARBOR_BATCHING_TEST = build/bin/test_arbor_batching
PBFT_RECOVERY_TEST = build/bin/test_pbft_recovery

all: $(BIN) $(SAGUARO_BIN) $(SHARPER_BIN) $(AHL_BIN)
$(BIN): source/main.cpp source/common.h source/network.h source/state_digest.h third_party/nlohmann/json.hpp
	mkdir -p build/bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) source/main.cpp $(LDFLAGS) $(LDLIBS) -o $@
$(SAGUARO_BIN): baseline/saguaro/main.cpp baseline/saguaro/protocol.inc source/main.cpp source/common.h source/network.h source/state_digest.h third_party/nlohmann/json.hpp
	mkdir -p build/bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) baseline/saguaro/main.cpp $(LDFLAGS) $(LDLIBS) -o $@
saguaro: $(SAGUARO_BIN)
$(AHL_BIN): baseline/ahl/main.cpp baseline/ahl/protocol.inc baseline/saguaro/protocol.inc source/main.cpp source/common.h source/network.h source/state_digest.h third_party/nlohmann/json.hpp
	mkdir -p build/bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) baseline/ahl/main.cpp $(LDFLAGS) $(LDLIBS) -o $@
ahl: $(AHL_BIN)
test-ahl: $(BIN) $(SAGUARO_BIN) $(AHL_BIN)
	python3 -B tests/test_ahl_tools.py
	python3 -B tests/test_ahl.py
$(SHARPER_BIN): baseline/sharper/main.cpp baseline/sharper/protocol.inc source/main.cpp source/common.h source/network.h source/state_digest.h third_party/nlohmann/json.hpp
	mkdir -p build/bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) baseline/sharper/main.cpp $(LDFLAGS) $(LDLIBS) -o $@
sharper: $(SHARPER_BIN)
test-sharper: $(BIN) $(SHARPER_BIN) $(SHARPER_RECOVERY_TEST)
	python3 -B tests/test_sharper_tools.py
	python3 -B tests/test_sharper_recovery.py
	python3 -B tests/test_sharper.py
$(SHARPER_RECOVERY_TEST): tests/test_sharper_recovery.cpp baseline/sharper/protocol.inc source/main.cpp source/common.h source/network.h source/state_digest.h third_party/nlohmann/json.hpp
	mkdir -p build/bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_sharper_recovery.cpp $(LDFLAGS) $(LDLIBS) -o $@
test-sharper-recovery: $(SHARPER_RECOVERY_TEST)
	python3 -B tests/test_sharper_recovery.py
$(PBFT_RECOVERY_TEST): tests/test_pbft_recovery.cpp baseline/saguaro/protocol.inc source/main.cpp source/common.h source/network.h source/state_digest.h third_party/nlohmann/json.hpp
	mkdir -p build/bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_pbft_recovery.cpp $(LDFLAGS) $(LDLIBS) -o $@
test-pbft-recovery: $(PBFT_RECOVERY_TEST)
	python3 -B tests/test_pbft_recovery.py
$(ARBOR_BATCHING_TEST): tests/test_arbor_batching.cpp source/main.cpp source/common.h source/network.h source/state_digest.h third_party/nlohmann/json.hpp
	mkdir -p build/bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_arbor_batching.cpp $(LDFLAGS) $(LDLIBS) -o $@
test-arbor-batching-unit: $(ARBOR_BATCHING_TEST)
	python3 -B tests/test_arbor_batching.py SelectionLogic
test-arbor-batching: $(BIN) $(ARBOR_BATCHING_TEST)
	python3 -B tests/test_arbor_batching.py
$(NETWORK_TEST): tests/test_network.cpp source/common.h source/network.h third_party/nlohmann/json.hpp
	mkdir -p build/bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Isource tests/test_network.cpp $(LDFLAGS) $(LDLIBS) -o $@
$(DIGEST_TEST): tests/test_state_digest.cpp source/common.h source/state_digest.h third_party/nlohmann/json.hpp
	mkdir -p build/bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Isource tests/test_state_digest.cpp $(LDFLAGS) $(LDLIBS) -o $@
test: $(BIN) $(SAGUARO_BIN) $(SHARPER_BIN) $(AHL_BIN) $(NETWORK_TEST) $(DIGEST_TEST) $(SHARPER_RECOVERY_TEST) $(ARBOR_BATCHING_TEST) $(PBFT_RECOVERY_TEST)
	$(NETWORK_TEST)
	$(DIGEST_TEST)
	python3 -B tests/test_pbft_recovery.py
	python3 -B tests/test_clean.py
	python3 tests/test_stage1.py
	python3 tests/test_stage2a.py
	python3 tests/test_stage2b.py
	python3 -B tests/test_multilayer.py
	python3 -B tests/test_arbor_batching.py
	python3 -B tests/test_client_fanout.py
	python3 -B tests/test_snapshot_digest.py
	python3 -B tests/test_engineering.py
	python3 -B tests/test_benchmark.py
	python3 -B tests/test_mixed_benchmark.py
	python3 -B tests/test_workload_locality.py
	python3 -B tests/test_compare_all.py
	python3 -B tests/test_method_tools.py
	python3 -B tests/test_saguaro.py
	python3 -B tests/test_sharper_tools.py
	python3 -B tests/test_sharper_recovery.py
	python3 -B tests/test_sharper.py
	python3 -B tests/test_ahl_tools.py
	python3 -B tests/test_ahl.py
benchmark: $(BIN)
	python3 -B scripts/benchmark.py --skip-build
check:
	python3 scripts/cluster.py validate --config config/two_layer.json
clean:
	python3 -B scripts/clean.py
.PHONY: all saguaro sharper ahl test-arbor-batching test-arbor-batching-unit test test-sharper test-sharper-recovery test-pbft-recovery test-ahl benchmark check clean
