# AXI2FLIT 的构建与回归入口；各位宽分别编译，参数通过命令行覆盖。
SYSTEMC_HOME ?= $(HOME)/.local/systemc-2.3.4-cxx17

CXX      := g++
CXXFLAGS := -std=c++17 -Wall -g \
            -I$(SYSTEMC_HOME)/include \
            -I./include
CXXFLAGS += $(EXTRA_CXXFLAGS) -I../../protocol/include
LDFLAGS  := -L$(SYSTEMC_HOME)/lib \
            -lsystemc -lpthread \
            -Wl,-rpath,$(SYSTEMC_HOME)/lib

SRC_DIR  := src
TB_DIR   := tb
SIM_DIR  := sim
INC_DIR  := include

# DUT 源文件（功能 TB 与性能 TB 共用）
DUT_SRCS := $(SRC_DIR)/flit_packer.cpp \
            $(SRC_DIR)/flit_unpacker.cpp \
            $(SRC_DIR)/axi2flit.cpp

FUNC_TB   := $(TB_DIR)/tb_axi2flit.cpp
PERF_TB   := $(TB_DIR)/tb_axi2flit_perf.cpp
GOLDEN_TB := $(TB_DIR)/tb_golden_vectors.cpp

HDRS := $(wildcard $(INC_DIR)/*.h) $(wildcard $(TB_DIR)/*.h)

# 默认位宽。命令行可覆盖：make WIDTH=1024 run
WIDTH  ?= 256
WIDTHS := 256 512 1024

TARGET        := $(SIM_DIR)/axi2flit_sim_$(WIDTH)
PERF_TARGET   := $(SIM_DIR)/axi2flit_perf_$(WIDTH)
GOLDEN_TARGET := $(SIM_DIR)/axi2flit_golden_$(WIDTH)

.PHONY: all run perf golden wave clean test-all perf-all golden-all report wire-all preflight
.PHONY: boundary-all adapter ucie-unit config-check reference-check FORCE

all: $(TARGET)

# 宏配置不体现在源码时间戳中；单项 run/perf/golden 也强制重编，避免修改
# EXTRA_CXXFLAGS（速率/TAT）后误用同位宽的旧二进制。
FORCE:

$(SIM_DIR):
	@mkdir -p $(SIM_DIR)

# ---- 默认位宽的功能仿真 ----
$(TARGET): $(DUT_SRCS) $(FUNC_TB) $(HDRS) FORCE | $(SIM_DIR)
	$(CXX) $(CXXFLAGS) -DAXI_DATA_WIDTH_CFG=$(WIDTH) $(DUT_SRCS) $(FUNC_TB) -o $@ $(LDFLAGS)
	@echo "编译成功：$@（AXI_DATA_WIDTH=$(WIDTH)）"

# ---- 默认位宽的性能仿真 ----
$(PERF_TARGET): $(DUT_SRCS) $(PERF_TB) $(HDRS) FORCE | $(SIM_DIR)
	$(CXX) $(CXXFLAGS) -DAXI_DATA_WIDTH_CFG=$(WIDTH) $(DUT_SRCS) $(PERF_TB) -o $@ $(LDFLAGS)
	@echo "编译成功：$@（AXI_DATA_WIDTH=$(WIDTH)）"

# ---- 黄金字节向量对拍（直接调用编码器，不推进仿真时间） ----
$(GOLDEN_TARGET): $(GOLDEN_TB) $(HDRS) FORCE | $(SIM_DIR)
	$(CXX) $(CXXFLAGS) -DAXI_DATA_WIDTH_CFG=$(WIDTH) $(GOLDEN_TB) -o $@ $(LDFLAGS)
	@echo "编译成功：$@（AXI_DATA_WIDTH=$(WIDTH)）"

run: $(TARGET)
	./$(TARGET)
	@echo ""
	@echo "波形文件：$(SIM_DIR)/waveform.vcd（make wave 查看）"

perf: $(PERF_TARGET)
	./$(PERF_TARGET)

golden: $(GOLDEN_TARGET)
	./$(GOLDEN_TARGET)

# ---- 三种位宽的功能回归 ----
# 位宽改变会改动 credit 深度与消息粒度，是最容易出回归的维度，必须全跑。
test-all: | $(SIM_DIR)
	@fail=0; \
	for w in $(WIDTHS); do \
	  echo "=================== 功能仿真 AXI_DATA_WIDTH=$$w ==================="; \
	  $(CXX) $(CXXFLAGS) -DAXI_DATA_WIDTH_CFG=$$w $(DUT_SRCS) $(FUNC_TB) \
	      -o $(SIM_DIR)/func_$$w $(LDFLAGS) || { fail=1; continue; }; \
	  ./$(SIM_DIR)/func_$$w > $(SIM_DIR)/func_$$w.log 2>&1; \
	  if [ $$? -eq 0 ]; then \
	    echo "  [PASS] $$w bit  （详细日志 $(SIM_DIR)/func_$$w.log）"; \
	    grep -E "^(Total|ALL TESTS|Accepted)" $(SIM_DIR)/func_$$w.log || true; \
	  else \
	    echo "  [FAIL] $$w bit  （详细日志 $(SIM_DIR)/func_$$w.log）"; \
	    grep -E "\[FAIL\]" $(SIM_DIR)/func_$$w.log | head -20; fail=1; \
	  fi; \
	done; \
	exit $$fail

# ---- 三种位宽的性能测量 ----
perf-all: | $(SIM_DIR)
	@fail=0; \
	for w in $(WIDTHS); do \
	  $(CXX) $(CXXFLAGS) -DAXI_DATA_WIDTH_CFG=$$w $(DUT_SRCS) $(PERF_TB) \
	      -o $(SIM_DIR)/perf_$$w $(LDFLAGS) || { fail=1; continue; }; \
	  ./$(SIM_DIR)/perf_$$w > $(SIM_DIR)/perf_$$w.log 2>&1 || fail=1; \
	  sed -n '/性能仿真配置/,$$p' $(SIM_DIR)/perf_$$w.log; \
	done; \
	exit $$fail

# ---- 三种位宽的消息黄金向量对拍 ----
# 将消息逐字节与独立固定向量比较，检查位域和数据排列。
golden-all: | $(SIM_DIR)
	@fail=0; \
	for w in $(WIDTHS); do \
	  echo "=============== 消息黄金向量对拍 AXI_DATA_WIDTH=$$w ==============="; \
	  $(CXX) $(CXXFLAGS) -DAXI_DATA_WIDTH_CFG=$$w $(GOLDEN_TB) \
	      -o $(SIM_DIR)/golden_$$w $(LDFLAGS) || { fail=1; continue; }; \
	  ./$(SIM_DIR)/golden_$$w || fail=1; \
	done; \
	exit $$fail

report: | $(SIM_DIR)
	@$(MAKE) --no-print-directory perf-all > $(SIM_DIR)/perf_report.txt 2>&1
	@echo "性能报告已生成：$(SIM_DIR)/perf_report.txt"

# 帧头、字节映射与流解析回归，与消息黄金向量分别统计。
wire-all: | $(SIM_DIR)
	@set -e; for w in $(WIDTHS); do \
	  $(CXX) $(CXXFLAGS) -DAXI_DATA_WIDTH_CFG=$$w $(TB_DIR)/tb_aou_wire.cpp \
	    -o $(SIM_DIR)/wire_$$w $(LDFLAGS); \
	  ./$(SIM_DIR)/wire_$$w; \
	done

preflight: reference-check
	$(MAKE) test-all
	$(MAKE) golden-all
	$(MAKE) wire-all
	$(MAKE) boundary-all
	$(MAKE) adapter
	$(MAKE) ucie-unit
	$(MAKE) config-check
	$(MAKE) perf-all

# 每种数据位宽测试 1/2/4 RP，并从 DUT 端口驱动 12 个明确的非法 AXI 场景。
boundary-all: | $(SIM_DIR)
	@set -e; for w in $(WIDTHS); do \
	  $(CXX) $(CXXFLAGS) -DAXI_DATA_WIDTH_CFG=$$w $(DUT_SRCS) $(TB_DIR)/tb_preintegration.cpp \
	    -o $(SIM_DIR)/boundary_$$w $(LDFLAGS); \
	  for rp in 1 2 4; do ./$(SIM_DIR)/boundary_$$w normal $$rp; done; \
	  for tc in aw_fixed ar_fixed aw_wrap ar_wrap aw_size ar_size aw_align ar_align aw_4k ar_4k w_early w_missing; do \
	    ./$(SIM_DIR)/boundary_$$w $$tc; \
	  done; \
	done

# 链路模型目录可独立配置；UCIE_DIR 的相对路径以 systemc/ 为基准。
AXI2FLIT_SYSTEMC_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
UCIE_DIR ?= ../../ucie-model
ADAPTER_FLAGS := -I./integration -I"$(UCIE_DIR)/src"
FULL_LINK_ARGS ?=
FULL_LINK_TARGET := $(SIM_DIR)/full_link_$(WIDTH)
.PHONY: full-link full-link-all full-link-negative full-link-wave
$(FULL_LINK_TARGET) $(SIM_DIR)/full_link_negative_$(WIDTH): $(DUT_SRCS) $(TB_DIR)/tb_full_link.cpp $(HDRS) $(wildcard integration/*.h) $(wildcard $(UCIE_DIR)/src/*.h) FORCE | $(SIM_DIR)
	$(CXX) $(CXXFLAGS) $(ADAPTER_FLAGS) -DAXI_DATA_WIDTH_CFG=$(WIDTH) $(DUT_SRCS) $(TB_DIR)/tb_full_link.cpp -o $@ $(LDFLAGS)

full-link: reference-check $(FULL_LINK_TARGET)
	./$(FULL_LINK_TARGET) $(FULL_LINK_ARGS)

# 将已有 VCD 转换为可独立打开的波形页面。
FULL_LINK_VCD ?= $(SIM_DIR)/full_link_$(WIDTH)_rp1.vcd
full-link-wave:
	python3 scripts/full_link_wave.py $(FULL_LINK_VCD)

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
	  AOU_INCLUDE="$(AXI2FLIT_SYSTEMC_DIR)/../../protocol/include" all unit-test

# 参数化回归：x8、PAM4、48 GB/s、TAT=80ns。
# 不复用默认配置的可执行文件，防止 make 未感知宏变化而命中旧缓存。
config-check: reference-check | $(SIM_DIR)
	$(CXX) $(CXXFLAGS) $(ADAPTER_FLAGS) -DAOU_LINK_LANES=8 -DAOU_LINK_BITS_PER_SYMBOL=2 \
	  -DAOU_LINK_TAT_NS=80 $(TB_DIR)/tb_ucie_adapter.cpp -o $(SIM_DIR)/adapter_config $(LDFLAGS)
	./$(SIM_DIR)/adapter_config
	$(CXX) $(CXXFLAGS) -DAOU_LINK_LANES=8 -DAOU_LINK_BITS_PER_SYMBOL=2 -DAOU_LINK_TAT_NS=80 \
	  $(DUT_SRCS) $(TB_DIR)/tb_preintegration.cpp -o $(SIM_DIR)/boundary_config $(LDFLAGS)
	./$(SIM_DIR)/boundary_config normal 1

# 两端源码均属于统一系统；这里只检查所需文件，不应用补丁。
reference-check:
	@test -f "$(UCIE_DIR)/src/ucie_link.h" -a -f "../../protocol/include/aou_format6.h" || \
	  { echo "缺少系统内的UCIe或公共协议头，请检查完整工作区。"; exit 1; }

wave:
	gtkwave $(SIM_DIR)/waveform.vcd &

clean:
	rm -f $(SIM_DIR)/full_link*
	rm -f $(SIM_DIR)/wire_* $(SIM_DIR)/boundary_* $(SIM_DIR)/adapter*
	rm -f $(SIM_DIR)/axi2flit_sim_* $(SIM_DIR)/axi2flit_perf_* $(SIM_DIR)/axi2flit_golden_*
	rm -f $(SIM_DIR)/axi2flit_sim $(SIM_DIR)/axi2flit_perf
	rm -f $(SIM_DIR)/func_* $(SIM_DIR)/perf_* $(SIM_DIR)/golden_*
	rm -f $(SIM_DIR)/waveform.vcd
	@echo "清理完成"
