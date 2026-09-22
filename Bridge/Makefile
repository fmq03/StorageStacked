# Bridge：介于 UCIe 链路与堆叠存储之间的协议转换/适配层。
#
# 标准版本的约束来自两个方向，且互相冲突：
#   * 系统 libsystemc 3.0.2 是用 C++17 编译的（导出 sc_api_version_..._cxx201703L），
#     任何以 C++20 编译并链接它的 TU 都会因 API 版本符号不匹配而链接失败。
#     => 所有碰 SystemC 的 TU 必须是 C++17。
#   * mem_sim 上游硬要求 C++20，但它的**静态库**由它自己的 CMake 以 C++20 构建。
#     消费方用什么标准是独立的；我们已经验证所需公开头在 C++17 下可编译。
#
# 因此这里的划分是：SystemC 侧 C++17，mem_sim 后端 TU 见 MEM_CXXSTD。
# 上游三个仓库全程只读；依赖由 scripts/vendor.sh 收集到 vendor/ 下。

SYSTEMC_HOME ?= /usr
MEM_SIM_DIR  ?= ../mem_sim
MEM_SIM_BUILD ?= build/mem_sim

# gem5 侧 CommMonitor 的 axi_data_width 必须 <= 本值，否则 AxSIZE 会超出
# AXI_SIZE_CODE，AouTarget 的 require 会直接 fatal。
AXI_DATA_WIDTH_CFG ?= 512

VENDOR      := vendor
AX2_INC     := $(VENDOR)/axi2flit/include
AX2_INT     := $(VENDOR)/axi2flit/integration
UCIE_INC    := $(VENDOR)/ucie-model/src
MEM_INC     := $(MEM_SIM_DIR)/include

# 碰 SystemC 的 TU 固定 C++17（见文件头说明）
SYSTEMC_STD ?= c++17
# mem_sim 后端 TU 不链接 SystemC，因此可以用与 mem_sim 库一致的 C++20
MEM_CXXSTD  ?= c++20

CXX      ?= g++
INCLUDES := -I$(SYSTEMC_HOME)/include -Iinclude -Isrc \
            -I$(AX2_INC) -I$(AX2_INT) -I$(UCIE_INC) -I$(MEM_INC) \
            -DAXI_DATA_WIDTH_CFG=$(AXI_DATA_WIDTH_CFG)
CXXFLAGS ?= -std=$(SYSTEMC_STD) -O2 -g -Wall -Wextra $(INCLUDES)

MEM_SIM_LIB := $(MEM_SIM_BUILD)/libhbm_sim_core.a
# 只有真正用到 mem_sim 的目标才链接它；桥自身的 smoke/回归不需要。
LDFLAGS     := -L$(SYSTEMC_HOME)/lib -lsystemc -lpthread \
               -Wl,-rpath,$(SYSTEMC_HOME)/lib
LDFLAGS_MEM := $(LDFLAGS) $(MEM_SIM_LIB)

BUILD := build
# Axi2Flit 的桥本体；链路其余部分都是纯头文件。
BRIDGE_SRCS := $(VENDOR)/axi2flit/src/flit_packer.cpp \
               $(VENDOR)/axi2flit/src/flit_unpacker.cpp \
               $(VENDOR)/axi2flit/src/axi2flit.cpp

.PHONY: all vendor mem-sim check-vendor test clean distclean help \
		rtl-lint rtl-test rtl-memsim-test rtl-full-link rtl-gem5-full-chain \
		rtl-diff rtl-model-src rtl-model-lib rtl

all: $(BUILD)/tb_vendor_smoke $(BUILD)/tb_backend_selftest $(BUILD)/tb_mem_sim_backend

help:
	@echo "make vendor    从上游收集依赖并打 AOU 补丁（先跑这个）"
	@echo "make mem-sim   编译 mem_sim 静态库 hbm_sim_core"
	@echo "make all       编译全部测试程序"
	@echo "make test      运行全部测试"
	@echo "make rtl-lint  对 Bridge RTL 执行 Verilator lint"
	@echo "make rtl-test  构建并运行 Bridge RTL 定向回归"
	@echo "make rtl-memsim-test  运行 RTL Bridge + 在线 mem_sim 回归"
	@echo "make rtl-full-link  运行 AXI/UCIe/RTL Bridge/mem_sim 联合仿真"
	@echo "make rtl-diff  对比 C++ Target 与 RTL Target 的确定性全链路摘要"
	@echo "make rtl-gem5-full-chain  构建 gem5 socket 到 RTL Bridge 的联合仿真"
	@echo "make rtl-model-src  生成供原生 gem5 编译的 Verilator C++ 模型"
	@echo "make rtl-model-lib  生成独立 C++ 程序可链接的 Verilator 静态库"
	@echo "make rtl       依次执行 rtl-lint 与 rtl-test"
	@echo "make clean     清理构建产物"

# ---------------------------------------------------------------- vendor

vendor:
	./scripts/vendor.sh

check-vendor:
	@test -f $(AX2_INC)/axi2flit.h || { \
	    echo "vendor/ 尚未就绪，请先运行 make vendor" >&2; exit 1; }
	@test -f $(UCIE_INC)/ucie_common.h || { \
	    echo "vendor/ 尚未就绪，请先运行 make vendor" >&2; exit 1; }

# ---------------------------------------------------------------- mem_sim

# mem_sim 没有 install/export，CMake 里也没有开关能跳过它自己的 CLI 与测试
# 可执行文件，所以只按 target 构建我们要的那一个静态库。
$(MEM_SIM_LIB): $(shell find $(MEM_SIM_DIR)/src $(MEM_SIM_DIR)/include -type f 2>/dev/null)
	@mkdir -p $(MEM_SIM_BUILD)
	cmake -S $(MEM_SIM_DIR) -B $(MEM_SIM_BUILD) -DCMAKE_BUILD_TYPE=Release >/dev/null
	cmake --build $(MEM_SIM_BUILD) --target hbm_sim_core -j $$(nproc)

mem-sim: $(MEM_SIM_LIB)
	@echo "hbm_sim_core 就绪：$(MEM_SIM_LIB)"

# ---------------------------------------------------------------- targets

$(BUILD)/tb_vendor_smoke: tb/tb_vendor_smoke.cpp $(BRIDGE_SRCS) | check-vendor
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# P0b：后端自证。不碰 SystemC，因此可以用与 mem_sim 一致的 C++20 编译。
MEM_CXXFLAGS := -std=$(MEM_CXXSTD) -O2 -g -Wall -Wextra \
                $(filter-out -I$(SYSTEMC_HOME)/include,$(INCLUDES)) \
                -Iinclude

$(BUILD)/tb_backend_selftest: tb/tb_backend_selftest.cpp src/mem_backend.cpp $(MEM_SIM_LIB) | check-vendor
	@mkdir -p $(BUILD)
	$(CXX) $(MEM_CXXFLAGS) tb/tb_backend_selftest.cpp src/mem_backend.cpp -o $@ $(MEM_SIM_LIB)

# mem_backend.cpp 单独以 C++20 编成 .o：它是唯一包含 mem_sim 头的 TU，而
# mem_sim 上游要求 C++20。SystemC 侧（tb 与 mem_sim_backend.cpp）必须是
# C++17，两边在链接期汇合——mem_backend.h 只暴露 C++17 干净的接口。
$(BUILD)/mem_backend.o: src/mem_backend.cpp include/mem_backend.h | check-vendor
	@mkdir -p $(BUILD)
	$(CXX) $(MEM_CXXFLAGS) -c src/mem_backend.cpp -o $@

$(BUILD)/tb_mem_sim_backend: tb/tb_mem_sim_backend.cpp src/mem_sim_backend.cpp \
                             $(BUILD)/mem_backend.o $(MEM_SIM_LIB) | check-vendor
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) tb/tb_mem_sim_backend.cpp src/mem_sim_backend.cpp \
	    $(BUILD)/mem_backend.o -o $@ $(LDFLAGS_MEM)

# P3a：gem5 在线事务协议回环。纯 C++，不碰 SystemC，也不碰 gem5 本体。
$(BUILD)/tb_socket_loopback: tb/tb_socket_loopback.cpp src/gem5_axi_socket.cpp \
                             src/gem5_axi_socket.h | check-vendor
	@mkdir -p $(BUILD)
	$(CXX) -std=c++17 -O2 -g -Wall -Wextra -pthread -Isrc \
	    tb/tb_socket_loopback.cpp src/gem5_axi_socket.cpp -o $@

# P2：把上游 tb_full_link 的后端换成 MemSimBackend，用它那套已验证的
# scoreboard 与 12 个用例来检验 Bridge。
SIM_DIR    := sim
FULL_LINK  := $(BUILD)/full_link_$(AXI_DATA_WIDTH_CFG)
FULL_LINK_ARGS ?=
FULL_LINK_TARGETS := $(foreach w,256 512 1024,$(BUILD)/full_link_$(w))

$(BUILD)/full_link_%: tb/tb_full_link.cpp src/mem_sim_backend.cpp \
                      $(BRIDGE_SRCS) $(BUILD)/mem_backend.o $(MEM_SIM_LIB) | check-vendor
	@mkdir -p $(BUILD) $(SIM_DIR)
	$(CXX) $(filter-out -DAXI_DATA_WIDTH_CFG=%,$(CXXFLAGS)) \
	    -DAXI_DATA_WIDTH_CFG=$* tb/tb_full_link.cpp \
	    src/mem_sim_backend.cpp $(BRIDGE_SRCS) $(BUILD)/mem_backend.o \
	    -o $@ $(LDFLAGS_MEM)

full-link: $(FULL_LINK)
	./$(FULL_LINK) $(FULL_LINK_ARGS)

# 每个位宽依次跑 RP=1/2/4，外加一次 RP=2 的压力+重放。任一失败即中止。
full-link-all:
	@for w in 256 512 1024; do \
	    $(MAKE) --no-print-directory $(BUILD)/full_link_$$w || exit 1; \
	    for rp in 1 2 4; do \
	        echo "--- width=$$w RP=$$rp ---"; \
	        ./$(BUILD)/full_link_$$w --rp $$rp > $(SIM_DIR)/full_link_$${w}_rp$${rp}.log 2>&1 || { \
	            echo "失败：width=$$w RP=$$rp"; tail -20 $(SIM_DIR)/full_link_$${w}_rp$${rp}.log; exit 1; }; \
	    done; \
	    echo "--- width=$$w RP=2 stress+replay ---"; \
	    ./$(BUILD)/full_link_$$w --rp 2 --stress --replay > $(SIM_DIR)/full_link_$${w}_rp2_stress_replay.log 2>&1 || { \
	        echo "失败：width=$$w stress+replay"; tail -20 $(SIM_DIR)/full_link_$${w}_rp2_stress_replay.log; exit 1; }; \
	done
	@echo "全链路（Bridge 后端）全部通过"

# P3b：gem5 → Bridge → mem_sim 完整链路。需要 gem5 已构建、并由外部先起
# CommMonitor 的监听 socket；见 scripts/run_gem5_chain.sh。
$(BUILD)/tb_gem5_full_chain: tb/tb_gem5_full_chain.cpp src/mem_sim_backend.cpp \
                             src/gem5_axi_agent.cpp src/gem5_axi_socket.cpp \
                             $(BRIDGE_SRCS) $(BUILD)/mem_backend.o $(MEM_SIM_LIB) | check-vendor
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) tb/tb_gem5_full_chain.cpp src/mem_sim_backend.cpp \
	    src/gem5_axi_agent.cpp src/gem5_axi_socket.cpp $(BRIDGE_SRCS) \
	    $(BUILD)/mem_backend.o -o $@ $(LDFLAGS_MEM)

# mock gem5：在真 gem5 接入前驱动整条链路，自带字节级参考模型。
$(BUILD)/mock_gem5_server: tools/mock_gem5_server.cpp | check-vendor
	@mkdir -p $(BUILD)
	$(CXX) -std=c++17 -O2 -g -Wall -Wextra $< -o $@

mock-chain:
	./scripts/run_mock_chain.sh

# ---------------------------------------------------------------- RTL Bridge

VERILATOR ?= verilator
RTL_DIR := rtl
RTL_TB_DIR := rtl_tb
RTL_SRCS := $(RTL_DIR)/bridge_pkg.sv \
	$(RTL_DIR)/fdi_rx_unpack.sv \
	$(RTL_DIR)/aou_msg_decode.sv \
	$(RTL_DIR)/storage_bridge_rx.sv
RTL_TOP_SRCS := $(RTL_SRCS) \
	$(RTL_DIR)/bridge_credit_mgr.sv \
	$(RTL_DIR)/bridge_fifo.sv \
	$(RTL_DIR)/bridge_req_buffer.sv \
	$(RTL_DIR)/bridge_scheduler.sv \
	$(RTL_DIR)/bridge_write_fsm.sv \
	$(RTL_DIR)/bridge_read_fsm.sv \
	$(RTL_DIR)/bridge_outstanding.sv \
	$(RTL_DIR)/bridge_reorder_buffer.sv \
	$(RTL_DIR)/bridge_response_gen.sv \
	$(RTL_DIR)/bridge_assertions.sv \
	$(RTL_DIR)/bridge_transaction_engine.sv \
	$(RTL_DIR)/fdi_tx_packer.sv \
	$(RTL_DIR)/storage_bridge_top.sv
RTL_WARN := -Wall -Wno-fatal -Wno-SYNCASYNCNET
VERILATOR_ROOT ?= $(shell $(VERILATOR) -V 2>/dev/null | \
	sed -n 's/^ *VERILATOR_ROOT *= *//p' | tail -1)
RTL_MODEL_DIR := $(BUILD)/rtl_model
RTL_MODEL_LIB := $(RTL_MODEL_DIR)/libstorage_bridge_rtl.a
RTL_MODEL_HEADER := $(RTL_MODEL_DIR)/Vstorage_bridge_top.h

rtl-lint:
	$(VERILATOR) --lint-only --timing $(RTL_WARN) \
		--top-module storage_bridge_top $(RTL_TOP_SRCS)

$(RTL_MODEL_HEADER): $(RTL_TOP_SRCS) Makefile
	@mkdir -p $(RTL_MODEL_DIR)
	$(VERILATOR) --cc --timing $(RTL_WARN) -GRP_COUNT=4 \
		--top-module storage_bridge_top \
		--Mdir $(RTL_MODEL_DIR) $(RTL_TOP_SRCS)

rtl-model-src: $(RTL_MODEL_HEADER)

$(RTL_MODEL_LIB): $(RTL_MODEL_HEADER)
	$(MAKE) -C $(RTL_MODEL_DIR) -f Vstorage_bridge_top.mk \
		CXX="$(CXX)" Vstorage_bridge_top__ALL.a
	$(CXX) -std=c++17 -O2 -I$(VERILATOR_ROOT)/include \
		-c $(VERILATOR_ROOT)/include/verilated.cpp -o $(RTL_MODEL_DIR)/verilated.o
	$(CXX) -std=c++17 -O2 -I$(VERILATOR_ROOT)/include \
		-c $(VERILATOR_ROOT)/include/verilated_threads.cpp \
		-o $(RTL_MODEL_DIR)/verilated_threads.o
	cp $(RTL_MODEL_DIR)/Vstorage_bridge_top__ALL.a $(RTL_MODEL_LIB)
	ar rcs $(RTL_MODEL_LIB) $(RTL_MODEL_DIR)/verilated.o \
		$(RTL_MODEL_DIR)/verilated_threads.o

rtl-model-lib: $(RTL_MODEL_LIB)

rtl-test: rtl-lint
	$(VERILATOR) --cc --exe --build --timing $(RTL_WARN) \
		--top-module storage_bridge_rx --Mdir $(BUILD)/obj_rtl_rx \
		-CFLAGS "-std=c++17 -O2" -LDFLAGS "-fuse-ld=bfd" $(RTL_SRCS) \
		$(RTL_TB_DIR)/tb_storage_bridge_rx.cpp
	./$(BUILD)/obj_rtl_rx/Vstorage_bridge_rx
	$(VERILATOR) --cc --exe --build --timing $(RTL_WARN) \
		--top-module storage_bridge_top --Mdir $(BUILD)/obj_rtl_top \
		-CFLAGS "-std=c++17 -O2" -LDFLAGS "-fuse-ld=bfd" $(RTL_TOP_SRCS) \
		$(RTL_TB_DIR)/tb_storage_bridge_top.cpp
	./$(BUILD)/obj_rtl_top/Vstorage_bridge_top
	$(VERILATOR) --cc --exe --build --timing $(RTL_WARN) -GRP_COUNT=2 \
		--top-module storage_bridge_top --Mdir $(BUILD)/obj_rtl_top_rp2 \
		-CFLAGS "-std=c++17 -O2 -DTEST_RP2" -LDFLAGS "-fuse-ld=bfd" \
		$(RTL_TOP_SRCS) $(RTL_TB_DIR)/tb_storage_bridge_top.cpp
	./$(BUILD)/obj_rtl_top_rp2/Vstorage_bridge_top
	$(VERILATOR) --cc --exe --build --timing $(RTL_WARN) -GRP_COUNT=2 \
		-GREQUEST_GRANULES=12 -GWDATA_GRANULES=64 \
		--top-module bridge_credit_mgr --Mdir $(BUILD)/obj_rtl_credit_rp2 \
		-CFLAGS "-std=c++17 -O2" -LDFLAGS "-fuse-ld=bfd" \
		$(RTL_DIR)/bridge_pkg.sv $(RTL_DIR)/bridge_credit_mgr.sv \
		$(RTL_TB_DIR)/tb_bridge_credit_mgr.cpp
	./$(BUILD)/obj_rtl_credit_rp2/Vbridge_credit_mgr
	$(VERILATOR) --cc --exe --build --timing $(RTL_WARN) \
		--top-module fdi_tx_packer --Mdir $(BUILD)/obj_rtl_tx \
		-CFLAGS "-std=c++17 -O2" -LDFLAGS "-fuse-ld=bfd" \
		$(RTL_DIR)/bridge_pkg.sv $(RTL_DIR)/fdi_tx_packer.sv \
		$(RTL_TB_DIR)/tb_fdi_tx_packer.cpp
	./$(BUILD)/obj_rtl_tx/Vfdi_tx_packer

rtl-memsim-test: rtl-lint $(MEM_SIM_LIB)
	$(VERILATOR) --cc --exe --build --timing $(RTL_WARN) \
		--top-module storage_bridge_top --Mdir $(BUILD)/obj_rtl_memsim \
		-CFLAGS "-std=c++20 -O2 -I$(abspath include) -I$(abspath $(MEM_INC))" \
		-LDFLAGS "-fuse-ld=bfd $(abspath $(MEM_SIM_LIB))" \
		$(RTL_TOP_SRCS) $(RTL_TB_DIR)/tb_storage_bridge_memsim.cpp \
		src/mem_backend.cpp
	./$(BUILD)/obj_rtl_memsim/Vstorage_bridge_top

# P6：用与 C++ Target 相同的 12 项 AXI/UCIe 测试驱动 RTL Bridge。Verilator
# 生成的模型、SystemC transactor 和 mem_sim 在同一进程、同一时间轴中运行。
RTL_FULL_LINK_OBJ := $(BUILD)/obj_rtl_full_link
RTL_FULL_LINK_BIN := $(RTL_FULL_LINK_OBJ)/Vstorage_bridge_top
RTL_FULL_LINK_ARGS ?= --rp 1

rtl-full-link: rtl-lint $(BUILD)/mem_backend.o $(MEM_SIM_LIB) | check-vendor
	@mkdir -p $(BUILD) $(SIM_DIR)
	$(VERILATOR) --cc --exe --build --timing $(RTL_WARN) \
		--top-module storage_bridge_top --Mdir $(RTL_FULL_LINK_OBJ) \
		-CFLAGS "-std=c++17 -O2 -DRTL_BRIDGE -DAXI_DATA_WIDTH_CFG=256 \
		-I$(abspath include) -I$(abspath src) -I$(abspath $(AX2_INC)) \
		-I$(abspath $(AX2_INT)) -I$(abspath $(UCIE_INC)) \
		-I$(abspath $(SYSTEMC_HOME)/include)" \
		-LDFLAGS "-fuse-ld=bfd $(abspath $(BUILD)/mem_backend.o) \
		$(abspath $(MEM_SIM_LIB)) -L$(abspath $(SYSTEMC_HOME)/lib) \
		-lsystemc -lpthread -Wl,-rpath,$(abspath $(SYSTEMC_HOME)/lib)" \
		$(RTL_TOP_SRCS) tb/tb_full_link.cpp src/rtl_bridge_systemc.cpp \
		$(BRIDGE_SRCS)
	./$(RTL_FULL_LINK_BIN) $(RTL_FULL_LINK_ARGS)

rtl-diff: $(BUILD)/full_link_256 rtl-full-link
	bash scripts/rtl_diff.sh

# P7：socket/gem5 入口切换到 RTL Bridge。该目标只构建；运行由
# scripts/run_mock_chain.sh 或 scripts/run_gem5_chain.sh 负责启动对端 server。
RTL_GEM5_OBJ := $(BUILD)/obj_rtl_gem5_chain
RTL_GEM5_BIN := $(RTL_GEM5_OBJ)/Vstorage_bridge_top

$(RTL_GEM5_BIN): $(RTL_TOP_SRCS) tb/tb_gem5_full_chain.cpp \
		src/rtl_bridge_systemc.cpp src/rtl_bridge_systemc.h \
		src/gem5_axi_agent.cpp src/gem5_axi_agent.h \
		src/gem5_axi_socket.cpp src/gem5_axi_socket.h \
		$(BRIDGE_SRCS) $(BUILD)/mem_backend.o $(MEM_SIM_LIB) | check-vendor
	@mkdir -p $(BUILD) $(SIM_DIR)
	$(VERILATOR) --cc --exe --build --timing $(RTL_WARN) \
		--top-module storage_bridge_top --Mdir $(RTL_GEM5_OBJ) \
		-CFLAGS "-std=c++17 -O2 -DRTL_BRIDGE -DAXI_DATA_WIDTH_CFG=256 \
		-I$(abspath include) -I$(abspath src) -I$(abspath $(AX2_INC)) \
		-I$(abspath $(AX2_INT)) -I$(abspath $(UCIE_INC)) \
		-I$(abspath $(SYSTEMC_HOME)/include)" \
		-LDFLAGS "-fuse-ld=bfd $(abspath $(BUILD)/mem_backend.o) \
		$(abspath $(MEM_SIM_LIB)) -L$(abspath $(SYSTEMC_HOME)/lib) \
		-lsystemc -lpthread -Wl,-rpath,$(abspath $(SYSTEMC_HOME)/lib)" \
		$(RTL_TOP_SRCS) tb/tb_gem5_full_chain.cpp src/rtl_bridge_systemc.cpp \
		src/gem5_axi_agent.cpp src/gem5_axi_socket.cpp $(BRIDGE_SRCS)

rtl-gem5-full-chain: rtl-lint $(RTL_GEM5_BIN)

rtl: rtl-lint rtl-test rtl-memsim-test rtl-full-link rtl-gem5-full-chain

test: all
	./$(BUILD)/tb_vendor_smoke
	./$(BUILD)/tb_backend_selftest
	./$(BUILD)/tb_mem_sim_backend

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf $(VENDOR)
