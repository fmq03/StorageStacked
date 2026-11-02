# Cross-component tests use the source/include manifest selected by the caller.
.DEFAULT_GOAL := full-link-all
STORAGE_ROOT := $(abspath ../..)
AXI2FLIT_SOURCE := $(shell python3 "$(STORAGE_ROOT)/scripts/storage_manifest.py" --field axi2flit_source)
PROTOCOL_INCLUDE := $(shell python3 "$(STORAGE_ROOT)/scripts/storage_manifest.py" --field protocol_include)
MANIFEST_INCLUDES := $(shell python3 "$(STORAGE_ROOT)/scripts/storage_manifest.py" --include-flags)
SYSTEMC_HOME ?= $(HOME)/.local/systemc-2.3.4-cxx17
SYSTEMC_CXX_STANDARD ?= 17
CXX ?= g++
CXXFLAGS := -std=c++$(SYSTEMC_CXX_STANDARD) -Wall -g -I$(SYSTEMC_HOME)/include $(EXTRA_CXXFLAGS)
LDFLAGS := -L$(SYSTEMC_HOME)/lib -L$(SYSTEMC_HOME)/lib64 -lsystemc -lpthread -Wl,-rpath,$(SYSTEMC_HOME)/lib -Wl,-rpath,$(SYSTEMC_HOME)/lib64
DUT_SRCS := $(shell python3 "$(STORAGE_ROOT)/scripts/storage_manifest.py" --sources axi2flit)
HDRS := $(wildcard $(AXI2FLIT_SOURCE)/systemc/include/*.h)
TB_DIR := .
SIM_DIR := sim
WIDTH ?= 256
WIDTHS := 256 512 1024
.PHONY: FORCE adapter ucie-unit config-check reference-check preflight
FORCE:
$(SIM_DIR):
	@mkdir -p $(SIM_DIR)
preflight: adapter ucie-unit config-check full-link-all full-link-negative

# 链路模型与协议头位置来自 storage_manifest.json。
AXI2FLIT_SYSTEMC_DIR := $(AXI2FLIT_SOURCE)/systemc
UCIE_DIR := $(shell python3 "$(STORAGE_ROOT)/scripts/storage_manifest.py" --field ucie_source)
ADAPTER_FLAGS := $(MANIFEST_INCLUDES)
FULL_LINK_ARGS ?=
FULL_LINK_TARGET := $(SIM_DIR)/full_link_$(WIDTH)
.PHONY: full-link full-link-all full-link-negative full-link-wave
$(FULL_LINK_TARGET) $(SIM_DIR)/full_link_negative_$(WIDTH): $(DUT_SRCS) $(TB_DIR)/tb_full_link.cpp $(HDRS) $(wildcard $(STORAGE_ROOT)/storage_axi/link/*.h) $(wildcard $(UCIE_DIR)/src/*.h) FORCE | $(SIM_DIR)
	$(CXX) $(CXXFLAGS) $(ADAPTER_FLAGS) -DAXI_DATA_WIDTH_CFG=$(WIDTH) $(DUT_SRCS) $(TB_DIR)/tb_full_link.cpp -o $@ $(LDFLAGS)

full-link: reference-check $(FULL_LINK_TARGET)
	./$(FULL_LINK_TARGET) $(FULL_LINK_ARGS)

# 将已有 VCD 转换为可独立打开的波形页面。
FULL_LINK_VCD ?= $(SIM_DIR)/full_link_$(WIDTH)_rp1.vcd
full-link-wave:
	python3 full_link_wave.py $(FULL_LINK_VCD)

# 每个位宽编译一次，各场景使用独立的波形、事务记录和日志路径。
full-link-all: reference-check | $(SIM_DIR)
	@set -e; for w in $(WIDTHS); do \
	  $(MAKE) --no-print-directory WIDTH=$$w $(SIM_DIR)/full_link_$$w; \
	  for rp in 1 2 4; do \
	    log=$(SIM_DIR)/full_link_$${w}_rp$${rp}.log; \
	    ./$(SIM_DIR)/full_link_$$w --rp $$rp > $$log 2>&1 || { tail -30 $$log; exit 1; }; \
	    tail -5 $$log; \
	  done; \
	  log=$(SIM_DIR)/full_link_$${w}_rp2_stress_replay.log; \
	  ./$(SIM_DIR)/full_link_$$w --rp 2 --stress --replay > $$log 2>&1 || { tail -30 $$log; exit 1; }; \
	  tail -5 $$log; \
	done

# 负向自检仅破坏记分板期望值；必须同时检出数据错误和失败退出状态。
full-link-negative: reference-check $(SIM_DIR)/full_link_negative_$(WIDTH)
	@rc=0; ./$(SIM_DIR)/full_link_negative_$(WIDTH) --negative-scoreboard > $(SIM_DIR)/full_link_negative.log 2>&1 || rc=$$?; \
	  test $$rc -eq 1 && grep -q 'R data mismatch' $(SIM_DIR)/full_link_negative.log && \
	  grep -q 'FULL-LINK TESTS FAILED' $(SIM_DIR)/full_link_negative.log && \
	  echo 'PASS: scoreboard negative control detected corrupted expectation'

adapter: reference-check | $(SIM_DIR)
	$(CXX) $(CXXFLAGS) $(ADAPTER_FLAGS) $(TB_DIR)/tb_ucie_adapter.cpp -o $(SIM_DIR)/adapter $(LDFLAGS)
	./$(SIM_DIR)/adapter

ucie-unit: reference-check
	$(MAKE) -C "$(UCIE_DIR)" SYSTEMC_HOME="$(SYSTEMC_HOME)" \
	  AOU_INCLUDE="$(PROTOCOL_INCLUDE)" all unit-test

# 参数化回归：x8、PAM4、48 GB/s、TAT=80ns。
# 不复用默认配置的可执行文件，防止 make 未感知宏变化而命中旧缓存。
config-check: reference-check | $(SIM_DIR)
	$(CXX) $(CXXFLAGS) $(ADAPTER_FLAGS) -DAOU_LINK_LANES=8 -DAOU_LINK_BITS_PER_SYMBOL=2 \
	  -DAOU_LINK_TAT_NS=80 $(TB_DIR)/tb_ucie_adapter.cpp -o $(SIM_DIR)/adapter_config $(LDFLAGS)
	./$(SIM_DIR)/adapter_config
# 两端源码均属于统一系统；这里只检查所需文件，不应用补丁。
reference-check:
	@test -f "$(UCIE_DIR)/src/ucie_link.h" -a -f "$(PROTOCOL_INCLUDE)/aou_format6.h" || \
	  { echo "缺少系统内的UCIe或公共协议头，请检查完整工作区。"; exit 1; }
