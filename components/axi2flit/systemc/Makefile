# AXI2FLIT 的构建与回归入口；各位宽分别编译，参数通过命令行覆盖。
SYSTEMC_HOME ?= $(HOME)/.local/systemc-2.3.4-cxx17

CXX      ?= g++
SYSTEMC_CXX_STANDARD ?= 17
CXXFLAGS := -std=c++$(SYSTEMC_CXX_STANDARD) -Wall -g \
            -I$(SYSTEMC_HOME)/include \
            -I./include
CXXFLAGS += $(EXTRA_CXXFLAGS) -I../../../common/interfaces
LDFLAGS  := -L$(SYSTEMC_HOME)/lib -L$(SYSTEMC_HOME)/lib64 \
            -lsystemc -lpthread \
            -Wl,-rpath,$(SYSTEMC_HOME)/lib -Wl,-rpath,$(SYSTEMC_HOME)/lib64

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
RP_CASES := 1 2 4
INVALID_CASES := aw_fixed ar_fixed aw_wrap ar_wrap aw_size ar_size aw_align ar_align aw_4k ar_4k w_early w_missing
WIDTH_CASES := func golden wire perf
SUMMARY_CASES := $(foreach w,$(WIDTHS),$(foreach c,$(WIDTH_CASES),$(c)-$(w)) $(foreach rp,$(RP_CASES),boundary-$(w)-normal-$(rp)) $(foreach c,$(INVALID_CASES),boundary-$(w)-$(c))) config
SUMMARY_EXPECTED := $(words $(SUMMARY_CASES))
PYTHON ?= python3
SUMMARY_DIR ?= $(CURDIR)/build/verification
CASE_RUNNER ?= $(PYTHON) $(abspath ../../../common/verif/summary.py)
SUMMARY_RUN = $(CASE_RUNNER) run --directory "$(SUMMARY_DIR)"

TARGET        := $(SIM_DIR)/axi2flit_sim_$(WIDTH)
PERF_TARGET   := $(SIM_DIR)/axi2flit_perf_$(WIDTH)
GOLDEN_TARGET := $(SIM_DIR)/axi2flit_golden_$(WIDTH)

.PHONY: all run perf golden wave clean test-all perf-all golden-all report wire-all preflight
.PHONY: boundary-all config-check reference-check FORCE

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
	  $(SUMMARY_RUN) --name func-$$w --require '^ALL TESTS PASSED$$' -- ./$(SIM_DIR)/func_$$w > $(SIM_DIR)/func_$$w.log 2>&1; \
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
	  $(SUMMARY_RUN) --name perf-$$w --require '^PERF RUN OK$$' -- ./$(SIM_DIR)/perf_$$w > $(SIM_DIR)/perf_$$w.log 2>&1 || fail=1; \
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
	  $(SUMMARY_RUN) --name golden-$$w --counts '检查项 ([0-9]+) 通过 / ([0-9]+) 失败' --require 'GOLDEN VECTORS MATCH' -- ./$(SIM_DIR)/golden_$$w || fail=1; \
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
	  $(SUMMARY_RUN) --name wire-$$w --counts 'WIRE .*: ([0-9]+) PASS / ([0-9]+) FAIL' -- ./$(SIM_DIR)/wire_$$w; \
	done

preflight: reference-check
	$(CASE_RUNNER) init --directory "$(SUMMARY_DIR)" --expected $(SUMMARY_EXPECTED)
	@status=0; for target in test-all golden-all wire-all boundary-all config-check perf-all; do \
	  $(MAKE) $$target SUMMARY_DIR="$(SUMMARY_DIR)" || { status=1; break; }; \
	done; \
	$(CASE_RUNNER) finish --directory "$(SUMMARY_DIR)" --expected $(SUMMARY_EXPECTED) --names $(SUMMARY_CASES) || status=1; \
	exit $$status

# 每种数据位宽测试 1/2/4 RP，并从 DUT 端口驱动 12 个明确的非法 AXI 场景。
boundary-all: | $(SIM_DIR)
	@set -e; for w in $(WIDTHS); do \
	  $(CXX) $(CXXFLAGS) -DAXI_DATA_WIDTH_CFG=$$w $(DUT_SRCS) $(TB_DIR)/tb_preintegration.cpp \
	    -o $(SIM_DIR)/boundary_$$w $(LDFLAGS); \
	  for rp in $(RP_CASES); do $(SUMMARY_RUN) --name boundary-$$w-normal-$$rp --pass-pattern '^BOUNDARY .*PASS$$' -- ./$(SIM_DIR)/boundary_$$w normal $$rp; done; \
	  for tc in $(INVALID_CASES); do \
	    $(SUMMARY_RUN) --name boundary-$$w-$$tc --pass-pattern '^BOUNDARY .*PASS$$' -- ./$(SIM_DIR)/boundary_$$w $$tc; \
	  done; \
	done

# Configuration-only bridge regression; it needs no UCIe implementation.
config-check: reference-check | $(SIM_DIR)
	$(CXX) $(CXXFLAGS) -DAOU_LINK_LANES=8 -DAOU_LINK_BITS_PER_SYMBOL=2 -DAOU_LINK_TAT_NS=80 \
	  $(DUT_SRCS) $(TB_DIR)/tb_preintegration.cpp -o $(SIM_DIR)/boundary_config $(LDFLAGS)
	$(SUMMARY_RUN) --name config --pass-pattern '^BOUNDARY .*PASS$$' -- ./$(SIM_DIR)/boundary_config normal 1

reference-check:
	@test -f "../../../common/interfaces/aou_format6.h" || { echo "缺少公共协议头"; exit 1; }

wave:
	gtkwave $(SIM_DIR)/waveform.vcd &

clean:
	rm -f $(SIM_DIR)/wire_* $(SIM_DIR)/boundary_*
	rm -f $(SIM_DIR)/axi2flit_sim_* $(SIM_DIR)/axi2flit_perf_* $(SIM_DIR)/axi2flit_golden_*
	rm -f $(SIM_DIR)/axi2flit_sim $(SIM_DIR)/axi2flit_perf
	rm -f $(SIM_DIR)/func_* $(SIM_DIR)/perf_* $(SIM_DIR)/golden_*
	rm -f $(SIM_DIR)/waveform.vcd
	@echo "清理完成"
