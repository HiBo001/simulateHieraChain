CXX ?= c++
OPENSSL_PREFIX ?= $(shell if command -v brew >/dev/null 2>&1; then brew --prefix openssl@3 2>/dev/null; fi)
CPPFLAGS += -Ithird_party $(if $(OPENSSL_PREFIX),-I$(OPENSSL_PREFIX)/include)
CXXFLAGS ?= -O2 -g -std=c++17 -Wall -Wextra -Wpedantic
ifneq ($(strip $(OPENSSL_PREFIX)),)
LDFLAGS += -L$(OPENSSL_PREFIX)/lib -Wl,-rpath,$(OPENSSL_PREFIX)/lib
endif
LDLIBS += -lcrypto -pthread
BIN = build/bin/arbor_node
NETWORK_TEST = build/bin/test_network
DIGEST_TEST = build/bin/test_state_digest

all: $(BIN)
$(BIN): source/main.cpp source/common.h source/network.h source/state_digest.h third_party/nlohmann/json.hpp
	mkdir -p build/bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) source/main.cpp $(LDFLAGS) $(LDLIBS) -o $@
$(NETWORK_TEST): tests/test_network.cpp source/common.h source/network.h third_party/nlohmann/json.hpp
	mkdir -p build/bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Isource tests/test_network.cpp $(LDFLAGS) $(LDLIBS) -o $@
$(DIGEST_TEST): tests/test_state_digest.cpp source/common.h source/state_digest.h third_party/nlohmann/json.hpp
	mkdir -p build/bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Isource tests/test_state_digest.cpp $(LDFLAGS) $(LDLIBS) -o $@
test: $(BIN) $(NETWORK_TEST) $(DIGEST_TEST)
	$(NETWORK_TEST)
	$(DIGEST_TEST)
	python3 -B tests/test_clean.py
	python3 tests/test_stage1.py
	python3 tests/test_stage2a.py
	python3 tests/test_stage2b.py
	python3 -B tests/test_client_fanout.py
	python3 -B tests/test_snapshot_digest.py
	python3 -B tests/test_engineering.py
	python3 -B tests/test_benchmark.py
benchmark: $(BIN)
	python3 -B scripts/benchmark.py --skip-build
check:
	python3 scripts/cluster.py validate --config config/two_layer.json
clean:
	python3 -B scripts/clean.py
.PHONY: all test benchmark check clean
