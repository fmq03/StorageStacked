# AXI2FLIT SystemC 模型 Makefile
#
# 用法：
#   make          编译并链接，生成可执行文件 sim/axi2flit_sim
#   make run      编译并运行仿真（生成 sim/waveform.vcd）
#   make wave     用 GTKWave 打开波形文件
#   make clean    清除所有编译产物

# ======================================================
#  SystemC 安装路径（与本机 /usr/local/systemc-2.3.4 对应）
# ======================================================
SYSTEMC_HOME ?= $(HOME)/.local/systemc-2.3.4-cxx17

# ======================================================
#  编译器与选项
# ======================================================
CXX      := g++
CXXFLAGS := -std=c++17 -Wall -g \
            -I$(SYSTEMC_HOME)/include \
            -I./include
LDFLAGS  := -L$(SYSTEMC_HOME)/lib \
            -lsystemc -lpthread \
            -Wl,-rpath,$(SYSTEMC_HOME)/lib

# ======================================================
#  源文件与目标
# ======================================================
SRC_DIR  := src
TB_DIR   := tb
SIM_DIR  := sim
INC_DIR  := include

SRCS := $(SRC_DIR)/flit_packer.cpp \
        $(SRC_DIR)/axi2flit.cpp    \
        $(TB_DIR)/tb_axi2flit.cpp

HDRS := $(wildcard $(INC_DIR)/*.h)

TARGET := $(SIM_DIR)/axi2flit_sim

# ======================================================
#  编译规则
# ======================================================
.PHONY: all run wave clean

all: $(SIM_DIR) $(TARGET)

$(SIM_DIR):
	mkdir -p $(SIM_DIR)

$(TARGET): $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)
	@echo ""
	@echo "编译成功：$(TARGET)"
	@echo "运行仿真：make run"

run: all
	./$(TARGET)
	@echo ""
	@echo "波形文件：$(SIM_DIR)/waveform.vcd"
	@echo "查看波形：make wave"

wave:
	gtkwave $(SIM_DIR)/waveform.vcd &

clean:
	rm -f $(TARGET)
	rm -f $(SIM_DIR)/waveform.vcd
	@echo "清理完成"
