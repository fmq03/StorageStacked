# ======================================================
#  AXI2FLIT SystemC 模型 Makefile
#
#  常用目标：
#    make            编译默认配置（256b）的功能仿真
#    make run        运行功能仿真（生成 sim/waveform.vcd）
#    make perf       运行默认配置的性能仿真（延迟/带宽报告）
#    make golden     运行规范一致性对拍（逐字节比对 AoU v0.8 黄金向量）
#    make test-all   编译并运行 256/512/1024 三种位宽的功能仿真
#    make perf-all   运行 256/512/1024 三种位宽的性能仿真
#    make golden-all 三种位宽的规范一致性对拍
#    make report     跑完 perf-all 并把报告汇总到 sim/perf_report.txt
#    make wave       用 GTKWave 打开波形
#    make clean      清除所有编译产物
#
#  AXI 数据位宽由 -DAXI_DATA_WIDTH_CFG 控制。位宽会同时影响：
#    - AoU DLENGTH 与三类数据消息的粒度数（8/14/27 等）
#    - 按 TAT 反推出的接收 FIFO 深度与初始 credit
#  因此每种位宽都必须单独编译一个可执行文件，不能共用 .o。
#
#  【可执行文件名必须带位宽】
#  曾经的写法是 sim/axi2flit_sim（不带位宽）。这样 make WIDTH=256 之后再
#  make WIDTH=1024，make 看到目标比依赖新就直接回答 "Nothing to be done"，
#  于是跑的还是上一次那个 256b 的二进制 —— 测量结果与标称位宽不符且毫无提示。
#  现在目标名一律带 $(WIDTH)，换位宽必然触发重新编译。
# ======================================================

SYSTEMC_HOME ?= $(HOME)/.local/systemc-2.3.4-cxx17

CXX      := g++
CXXFLAGS := -std=c++17 -Wall -g \
            -I$(SYSTEMC_HOME)/include \
            -I./include
CXXFLAGS += $(EXTRA_CXXFLAGS)
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
.PHONY: boundary-all endpoint ucie-unit config-check reference-check FORCE

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

# ---- 黄金字节向量对拍（不依赖 SystemC 内核，纯静态编译） ----
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

# ---- 三种位宽的规范一致性对拍 ----
# 与 test-all/perf-all 不同，本目标把消息逐字节和《AoU v0.8》表/图手工
# 展开的黄金向量比对，任何位域挪动都会立即暴露。
golden-all: | $(SIM_DIR)
	@fail=0; \
	for w in $(WIDTHS); do \
	  echo "=============== 规范一致性对拍 AXI_DATA_WIDTH=$$w ==============="; \
	  $(CXX) $(CXXFLAGS) -DAXI_DATA_WIDTH_CFG=$$w $(GOLDEN_TB) \
	      -o $(SIM_DIR)/golden_$$w $(LDFLAGS) || { fail=1; continue; }; \
	  ./$(SIM_DIR)/golden_$$w || fail=1; \
	done; \
	exit $$fail

report: | $(SIM_DIR)
	@$(MAKE) --no-print-directory perf-all > $(SIM_DIR)/perf_report.txt 2>&1
	@echo "性能报告已生成：$(SIM_DIR)/perf_report.txt"

# 接入前门禁：新增格式/解析回归单列，避免把消息 golden 的 71 项虚报成 PH 覆盖。
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
	$(MAKE) endpoint
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

# 组件单测引用 UCIe 公共类型和格式函数，不实例化 UcieLink/PHY。
UCIE_DIR := ../reference/ucie-model
ENDPOINT_FLAGS := -I./integration -I$(UCIE_DIR)/src
endpoint: reference-check | $(SIM_DIR)
	$(CXX) $(CXXFLAGS) $(ENDPOINT_FLAGS) $(TB_DIR)/tb_ucie_endpoint.cpp -o $(SIM_DIR)/endpoint $(LDFLAGS)
	./$(SIM_DIR)/endpoint

ucie-unit: reference-check
	$(MAKE) -C $(UCIE_DIR) SYSTEMC_HOME=$(SYSTEMC_HOME) all unit-test

# 参数替换也实际编译并运行：x8 PAM4 仍是 48 GB/s，TAT 改为 80ns。
# 不复用默认配置的可执行文件，防止 make 未感知宏变化而命中旧缓存。
config-check: reference-check | $(SIM_DIR)
	$(CXX) $(CXXFLAGS) $(ENDPOINT_FLAGS) -DAOU_LINK_LANES=8 -DAOU_LINK_BITS_PER_SYMBOL=2 \
	  -DAOU_LINK_TAT_NS=80 $(TB_DIR)/tb_ucie_endpoint.cpp -o $(SIM_DIR)/endpoint_config $(LDFLAGS)
	./$(SIM_DIR)/endpoint_config
	$(CXX) $(CXXFLAGS) -DAOU_LINK_LANES=8 -DAOU_LINK_BITS_PER_SYMBOL=2 -DAOU_LINK_TAT_NS=80 \
	  $(DUT_SRCS) $(TB_DIR)/tb_preintegration.cpp -o $(SIM_DIR)/boundary_config $(LDFLAGS)
	./$(SIM_DIR)/boundary_config normal 1

# reference 不纳入主仓库。反向检查只读确认交付补丁已安装，缺失时给出操作入口。
reference-check:
	@git -C $(UCIE_DIR) apply --reverse --check ../../systemc/integration/ucie-model-aou.patch || \
	  { echo "UCIe 接入补丁未安装或与本地修改冲突，请按 integration/README.md 检查。"; exit 1; }

wave:
	gtkwave $(SIM_DIR)/waveform.vcd &

clean:
	rm -f $(SIM_DIR)/wire_* $(SIM_DIR)/boundary_* $(SIM_DIR)/endpoint*
	rm -f $(SIM_DIR)/axi2flit_sim_* $(SIM_DIR)/axi2flit_perf_* $(SIM_DIR)/axi2flit_golden_*
	rm -f $(SIM_DIR)/axi2flit_sim $(SIM_DIR)/axi2flit_perf
	rm -f $(SIM_DIR)/func_* $(SIM_DIR)/perf_* $(SIM_DIR)/golden_*
	rm -f $(SIM_DIR)/waveform.vcd
	@echo "清理完成"
