# 顶层 Makefile —— 只做本仓库自己的事。
#
# 三个 XPU 仿真器与外部 mem_sim 的构建**不在**这里：gem5 用 SCons、
# CoralNPU 用 bazel、Vortex 与 mem_sim 各用自己的构建系统。要构建它们
# 见项目手册 docs/USER_MANUAL.md；补丁与实现细节见 docs/04-integration.md。
#
# 这里能做的是"不需要任何仿真器就能验的部分"：生成物是否与 addrmap.json 同步、各项
# 自测是否过。CI 应该先跑 `make check`，因为它快且能挡掉最隐晦的一类错误。

PROJ    := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
PYTHON  ?= python3
CXX     ?= g++
BUILD   := build

.PHONY: all check check-addrmap addrmap test test-tools test-writer test-workflow \
        test-storage-chain test-memsim-smoke \
        benchmark-llm-memory preflight \
        install install-gem5 install-vortex \
        install-coralnpu clean help

all: check

help:
	@echo "make check          - addrmap + 工具/writer/源码获取自测（不需要仿真器）"
	@echo "make addrmap        - 从 addrmap.json 重新生成 C++/Python 侧的地址表"
	@echo "make check-addrmap  - 只校验生成物是否过期，不写文件"
	@echo "make test           - test-tools + test-writer + test-workflow"
	@echo "make test-storage-chain - 透明 AXI4 边界 + trace/checker RTL 回归"
	@echo "make test-memsim-smoke - 小型 HETTrace -> 外部 hbm_sim 端到端回归"
	@echo "make benchmark-llm-memory - 合成 decoder-LLM 访存流 -> 外部 mem_sim/hbm_sim"
	@echo "make preflight      - 只读检查原生 Linux 全流程所需命令、源码树和产物"
	@echo "make install        - 把本项目装进三棵树（需要 GEM5_HOME 等环境变量）"
	@echo "make clean          - 删掉本项目生成的 build 产物与 Python 缓存"
	@echo ""
	@echo "仿真器的构建与端到端测试见项目手册 docs/USER_MANUAL.md"

check: check-addrmap test

# ---- addrmap ---------------------------------------------------------------
# 生成物过期是本仓库里最隐晦的一类错误：C++ 侧和 Python 侧会对"哪个地址属于哪个
# 区域"给出不同答案，而两边都不报错。所以 check 的第一步就是它。
addrmap:
	$(PYTHON) scripts/gen_addrmap.py

check-addrmap:
	@$(PYTHON) scripts/gen_addrmap.py --check

# ---- 自测 ------------------------------------------------------------------
test: test-tools test-writer test-workflow

test-workflow:
	@$(PYTHON) tools/tests/test_workflow.py

test-storage-chain:
	$(MAKE) -C storage_chain test lint

# ---- benchmark -------------------------------------------------------------
test-memsim-smoke:
	LLM_BENCH_OUT=$(BUILD)/llm_memory_smoke ./workloads/llm_memory/run.sh \
		--hidden-size 16 --layers 1 --context-tokens 8 --decode-tokens 2 \
		--ffn-multiplier 2 --request-bytes 16

benchmark-llm-memory:
	./workloads/llm_memory/run.sh

preflight:
	./scripts/native_preflight.sh

# 不用 pytest：少一个依赖，在只有 gem5 自带 python 的机器上也能跑。脚本自己数
# 检查项、自己定退出码。
test-tools:
	@PYTHONPATH=$(PROJ)/tools $(PYTHON) tools/tests/test_tools.py

test-writer: $(BUILD)/test_writer
	@$(BUILD)/test_writer

$(BUILD)/test_writer: libhettrace/tests/test_writer.cc \
                      $(wildcard libhettrace/include/hettrace/*.h) | $(BUILD)
	$(CXX) -std=c++17 -Wall -Wextra -Ilibhettrace/include -o $@ $<

$(BUILD):
	mkdir -p $@

# ---- 安装 ------------------------------------------------------------------
# 四步必须严格串行。尤其是 Vortex 的 gem5 SimObject source-of-truth 在 Vortex 树里：
# 必须先给 Vortex 打补丁，再运行 Vortex 自带的 gem5 installer，随后才能安装本项目
# gem5 增量；即使用户以 `make -j install` 调用，这个单一 recipe 也不会乱序。
install:
	@set -eu; \
	workspace_root="$(dir $(PROJ))"; \
	vortex_home="$${VORTEX_HOME:-$${workspace_root}vortex-gpu/vortex}"; \
	gem5_home="$${GEM5_HOME:-$${workspace_root}gem5}"; \
	coralnpu_home="$${CORALNPU_HOME:-$${workspace_root}coralnpu}"; \
	[ -x "$$vortex_home/sim/simx/gem5/install.sh" ] || { \
		echo "错误: 找不到 Vortex gem5 installer: $$vortex_home/sim/simx/gem5/install.sh" >&2; \
		exit 1; \
	}; \
	echo "[1/4] 安装 Vortex 项目增量"; \
	VORTEX_HOME="$$vortex_home" ./vortexint/install.sh; \
	echo "[2/4] 把 Vortex gem5 SimObject 安装进 gem5"; \
	VORTEX_HOME="$$vortex_home" GEM5_HOME="$$gem5_home" \
		"$$vortex_home/sim/simx/gem5/install.sh"; \
	echo "[3/4] 安装统一 gem5 增量"; \
	GEM5_HOME="$$gem5_home" ./gem5int/install.sh; \
	echo "[4/4] 安装 CoralNPU 项目增量"; \
	CORALNPU_HOME="$$coralnpu_home" ./coralnpuint/install.sh

install-gem5:
	./gem5int/install.sh

# Vortex 要跑两个脚本且顺序不能反：先给 Vortex 树打补丁，再把打过补丁的 gem5 侧
# 源码装进 gem5（那份源码的 source-of-truth 在 Vortex 树里）。理由见
# docs/04-integration.md。Vortex 自带的 third_party/ramulator 是 SimX 依赖，
# 由 Vortex 构建流程管理，不是本项目的在线内存后端。
install-vortex:
	@set -eu; \
	workspace_root="$(dir $(PROJ))"; \
	vortex_home="$${VORTEX_HOME:-$${workspace_root}vortex-gpu/vortex}"; \
	gem5_home="$${GEM5_HOME:-$${workspace_root}gem5}"; \
	[ -x "$$vortex_home/sim/simx/gem5/install.sh" ] || { \
		echo "错误: 找不到 Vortex gem5 installer: $$vortex_home/sim/simx/gem5/install.sh" >&2; \
		exit 1; \
	}; \
	VORTEX_HOME="$$vortex_home" ./vortexint/install.sh; \
	VORTEX_HOME="$$vortex_home" GEM5_HOME="$$gem5_home" \
		"$$vortex_home/sim/simx/gem5/install.sh"

install-coralnpu:
	./coralnpuint/install.sh

# 逐个删自己的产物，不是 `rm -rf $(BUILD)`。$(BUILD) 是 .gitignore 里的目录，别人
# （比如在项目里顺手跑一次 gem5 的 scons）完全可能往里放几个 G 的中间产物，一句
# rm -rf 会把那些一起端掉，而 make clean 是所有人都会随手敲的命令。
# 加了新产物就在这里加一行；rmdir 不带 -p、失败也不算错，目录非空就说明里面还有
# 不属于我们的东西，那就该留着。
clean:
	rm -f $(BUILD)/test_writer
	rm -rf $(BUILD)/llm_memory
	rm -rf $(BUILD)/llm_memory_smoke
	$(MAKE) -C storage_chain clean
	$(MAKE) -C workloads/shared_buffer clean
	$(MAKE) -C workloads/three_source clean
	$(MAKE) -C workloads/vortex_smoke clean
	rm -rf gem5int/configs/het/__pycache__ gem5int/src/hettrace/__pycache__
	rm -rf scripts/__pycache__ tools/hettrace/__pycache__ tools/tests/__pycache__
	rm -rf workloads/llm_memory/__pycache__
	-@rmdir $(BUILD) 2>/dev/null || true
