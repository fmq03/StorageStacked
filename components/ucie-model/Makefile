CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
LDLIBS ?= -lsystemc

# 与协议桥共用 SystemC 安装和物理帧字节映射。
SYSTEMC_HOME ?= $(HOME)/.local/systemc-2.3.4-cxx17
AOU_INCLUDE ?= ../protocol/include
CXXFLAGS += -I$(SYSTEMC_HOME)/include -I$(AOU_INCLUDE)
LDLIBS += -L$(SYSTEMC_HOME)/lib -Wl,-rpath,$(SYSTEMC_HOME)/lib -lpthread

BUILD := build
BIN := $(BUILD)/ucie_sc_sim
UNIT_BIN := $(BUILD)/ucie_unit_tests
SRC := src/ucie_systemc_main.cpp
UNIT_SRC := src/ucie_unit_tests.cpp
HDR := src/ucie_common.h src/ucie_fdi.h src/ucie_phy.h src/ucie_link.h $(AOU_INCLUDE)/aou_format6.h

.PHONY: all run unit-test test sweep validate clean

all: $(BIN) $(UNIT_BIN)

$(BIN): $(SRC) $(HDR)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(BIN) $(LDLIBS)

$(UNIT_BIN): $(UNIT_SRC) $(HDR)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(UNIT_SRC) -o $(UNIT_BIN) $(LDLIBS)

run: $(BIN)
	./$(BIN)

unit-test: $(UNIT_BIN)
	./$(UNIT_BIN)

test: $(BIN) $(UNIT_BIN)
	./$(UNIT_BIN)
	./scripts/run_tests.sh

sweep: $(BIN)
	./scripts/run_sweep.sh

validate: all
	./scripts/run_validation.sh

clean:
	rm -rf $(BUILD)
