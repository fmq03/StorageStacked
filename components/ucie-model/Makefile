CXX ?= g++
SYSTEMC_CXX_STANDARD ?= 17
CXXFLAGS ?= -std=c++$(SYSTEMC_CXX_STANDARD) -O2 -Wall -Wextra
LDLIBS ?= -lsystemc

# 与协议桥共用 SystemC 安装和物理帧字节映射。
SYSTEMC_HOME ?= $(HOME)/.local/systemc-2.3.4-cxx17
AOU_INCLUDE ?= ../../common/interfaces
CXXFLAGS += -I$(SYSTEMC_HOME)/include -I$(AOU_INCLUDE)
LDLIBS += -L$(SYSTEMC_HOME)/lib -L$(SYSTEMC_HOME)/lib64 -Wl,-rpath,$(SYSTEMC_HOME)/lib -Wl,-rpath,$(SYSTEMC_HOME)/lib64 -lpthread

BUILD := build
BIN := $(BUILD)/ucie_sc_sim
UNIT_BIN := $(BUILD)/ucie_unit_tests
SRC := src/ucie_systemc_main.cpp
UNIT_SRC := src/ucie_unit_tests.cpp
HDR := src/ucie_common.h src/ucie_fdi.h src/ucie_phy.h src/ucie_link.h $(AOU_INCLUDE)/aou_format6.h
PYTHON ?= python3
SUMMARY_DIR ?= $(CURDIR)/build/validation
OUT ?= $(SUMMARY_DIR)
CASE_RUNNER ?= $(PYTHON) $(abspath ../../common/verif/summary.py)
SWEEP_RATES := 24 32 48
SWEEP_NOISE := 0.00 0.05 0.10 0.15 0.18 0.20
SWEEP_JITTER := 0.00 0.01 0.03 0.05
SWEEP_SEEDS := 7 11 29
SWEEP_LANES := 8 16 32 64
NRZ_RATES := 24 32
SWEEP_CASES := $(foreach r,$(SWEEP_RATES),$(foreach s,$(SWEEP_NOISE),pam4_r$(r)_s$(s))) $(foreach j,$(SWEEP_JITTER),$(foreach s,$(SWEEP_SEEDS),jitter_$(j)_seed_$(s))) $(foreach l,$(SWEEP_LANES),lanes_$(l)) $(foreach r,$(NRZ_RATES),nrz_r$(r))
VALIDATION_CASES := unit functional $(SWEEP_CASES)
SUMMARY_EXPECTED := $(words $(VALIDATION_CASES))
export PYTHON SUMMARY_DIR OUT CASE_RUNNER SWEEP_RATES SWEEP_NOISE SWEEP_JITTER SWEEP_SEEDS SWEEP_LANES NRZ_RATES VALIDATION_CASES SUMMARY_EXPECTED

.PHONY: all run unit-test test sweep validate clean FORCE

all: $(BIN) $(UNIT_BIN)

$(BIN): $(SRC) $(HDR) FORCE
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(BIN) $(LDLIBS)

$(UNIT_BIN): $(UNIT_SRC) $(HDR) FORCE
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

FORCE:

clean:
	rm -rf $(BUILD)
