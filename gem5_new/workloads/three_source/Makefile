# 三源负载的 host 侧。
#
#   VORTEX_HOME=$HOME/vortex-gpu/vortex VORTEX_BUILD=$HOME/vortex-gpu/vxbuild make
#   make clean
#
# 与 ../shared_buffer/Makefile 的两处刻意不同：
#
# 1. **不是 -static**。那个负载只碰 PIO 和共享内存，静态链接能把 ld.so 的噪声全部
#    去掉；这个要用 Vortex 的 host runtime（libvortex.so），而那份 .so 自己再
#    dlopen 后端（libvortex-gem5-x86_64.so），静态链接根本走不通。所以这里跟上游
#    的 vecadd 一样是动态链接的，运行时靠 het_system.py 的 --vortex-host-rt-dir
#    把 LD_LIBRARY_PATH 设好。
#
# 2. 用 g++ 而不是 gcc：vortex2.h 本身是 C 的（有 extern "C"），但本负载用了
#    std::vector 来放 host 侧的 float 缓冲。
#
# -O2 照样安全：所有共享区和寄存器访问都是 volatile 的。

VORTEX_HOME  ?= $(HOME)/vortex-gpu/vortex
VORTEX_BUILD ?= $(HOME)/vortex-gpu/vxbuild

CXX      ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra -std=c++17
BUILD    := build

RT_DIR   := $(VORTEX_BUILD)/sw/runtime
INCS     := -I$(VORTEX_HOME)/sw/runtime/include
# -rpath 只是备用：gem5 SE 模式下 ld.so 在被仿真的进程里跑，会认 RUNPATH。真正生
# 效的通常是 het_system.py 设的 LD_LIBRARY_PATH，两条路都留着不冲突。
LIBS     := -L$(RT_DIR) -lvortex -Wl,-rpath,$(RT_DIR)

all: $(BUILD)/host_main

$(BUILD)/host_main: host_main.cpp | $(BUILD) check-rt
	$(CXX) $(CXXFLAGS) $(INCS) -o $@ $< $(LIBS)

# 缺 host runtime 时报一句能照着做的话，而不是让编译器抛 "cannot find -lvortex"
# —— 后者看不出该去 make 哪个目录。
check-rt:
	@test -f $(VORTEX_HOME)/sw/runtime/include/vortex2.h || { \
	    echo "错误: 找不到 $(VORTEX_HOME)/sw/runtime/include/vortex2.h"; \
	    echo "      VORTEX_HOME 指对了吗？"; exit 1; }
	@test -f $(RT_DIR)/libvortex.so || { \
	    echo "错误: 找不到 $(RT_DIR)/libvortex.so"; \
	    echo "      先建 Vortex 的 host runtime: make -C $(RT_DIR)"; exit 1; }

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

.PHONY: all clean check-rt
