CXX ?= c++
OPENSSL_PREFIX ?= $(shell if command -v brew >/dev/null 2>&1; then brew --prefix openssl@3 2>/dev/null; fi)
CPPFLAGS += -Ithird_party $(if $(OPENSSL_PREFIX),-I$(OPENSSL_PREFIX)/include)
CXXFLAGS ?= -O2 -g -std=c++17 -Wall -Wextra -Wpedantic
ifneq ($(strip $(OPENSSL_PREFIX)),)
LDFLAGS += -L$(OPENSSL_PREFIX)/lib -Wl,-rpath,$(OPENSSL_PREFIX)/lib
endif
LDLIBS += -lcrypto -pthread
BIN = build/bin/arbor_node

all: $(BIN)
$(BIN): source/main.cpp source/common.h source/network.h third_party/nlohmann/json.hpp
	mkdir -p build/bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) source/main.cpp $(LDFLAGS) $(LDLIBS) -o $@
test: $(BIN)
	python3 tests/test_stage1.py
check:
	python3 scripts/cluster.py validate --config config/two_layer.json
clean:
	rm -f $(BIN)
.PHONY: all test check clean
