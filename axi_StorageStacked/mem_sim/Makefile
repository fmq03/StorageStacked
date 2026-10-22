CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -pedantic
CPPFLAGS ?= -Iinclude
LDFLAGS ?=
LDLIBS ?=

# build/ 下只放编译产物；src/ 下按职责分层，object 文件保持相同子目录结构。
BUILD_DIR ?= build
TARGET := $(BUILD_DIR)/hbm_sim
SEQUENCE_TEST := $(BUILD_DIR)/sequence_tests
TIMING_BOUNDARY_TEST := $(BUILD_DIR)/timing_boundary_tests
PHY_TEST := $(BUILD_DIR)/phy_tests
CONFIG_TEST := $(BUILD_DIR)/config_tests
MODEL_CONFIG_TEST := $(BUILD_DIR)/model_config_tests
SCHEDULER_CONTRACT_TEST := $(BUILD_DIR)/scheduler_contract_tests
RESULT_VALUE_TEST := $(BUILD_DIR)/result_value_tests
REFACTOR_TEST := $(BUILD_DIR)/refactor_tests
# 主程序需要 src/cli/main.cpp；测试程序需要复用库代码但不能链接 CLI main。
SRCS := $(shell find src -name '*.cpp' | sort)
LIB_SRCS := $(filter-out src/cli/main.cpp,$(SRCS))
OBJS := $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))
LIB_OBJS := $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(LIB_SRCS))
DEPS := $(OBJS:.o=.d)

# Make 不会自动把编译器和 flags 的变化视为 object 依赖。把完整构建身份编码
# 到 stamp 文件名后，CXX/CPPFLAGS/CXXFLAGS/LDFLAGS/LDLIBS 任一变化都会让
# object 和测试程序重新生成，避免普通 G++ 链接到旧 ASan/UBSan object。
CXX_ID := $(shell $(CXX) --version 2>/dev/null | sed -n '1p')
BUILD_CONFIG_KEY := $(shell printf '%s\n' '$(CXX)' '$(CXX_ID)' '$(CPPFLAGS)' '$(CXXFLAGS)' '$(LDFLAGS)' '$(LDLIBS)' | cksum | awk '{print $$1}')
BUILD_CONFIG := $(BUILD_DIR)/.build-config-$(BUILD_CONFIG_KEY)

SANITIZER_BUILD_DIR ?= build-clang-sanitize
SANITIZER_CXX ?= /usr/bin/clang++-18
SANITIZER_FLAGS ?= -std=c++20 -O0 -g -Wall -Wextra -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer
SANITIZER_LDFLAGS ?= -fsanitize=address,undefined

.PHONY: all clean clean-build clean-outputs delete list-builds sanitize sanitize-test run test smoke sequence-test phy-test config-test phy-smoke visualization-smoke multistack-backend-smoke multistack-demos-smoke architecture-sweep-smoke architecture-sweep visualize-example timing-boundary-validation \
	model-validation performance-validation sensitivity-validation reference-validation ramulator-validation \
	examples examples_hbm4 examples_hbm3 examples_lpddr6 examples_lpddr5

RAMULATOR2_ROOT ?= /path/to/ramulator2

all: $(TARGET)

# 链接单个命令行工具。项目没有外部依赖，因此只需要把所有 object 合并即可。
$(TARGET): $(OBJS) $(BUILD_CONFIG) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(OBJS) $(LDLIBS) -o $@

# 每个 src/**/*.cpp 独立编译，方便增量构建；object 目录镜像源码分层。
$(BUILD_DIR)/%.o: src/%.cpp $(BUILD_CONFIG) | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

# sequence_tests 直接链接控制器库对象，用 C++ assert-style 测试检查协议序列。
$(SEQUENCE_TEST): $(LIB_OBJS) tests/sequence_tests.cpp tests/spec_fixture.hpp $(BUILD_CONFIG) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) $(LIB_OBJS) tests/sequence_tests.cpp $(LDLIBS) -o $@

$(TIMING_BOUNDARY_TEST): $(LIB_OBJS) tests/timing_boundary_tests.cpp tests/spec_fixture.hpp $(BUILD_CONFIG) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) $(LIB_OBJS) tests/timing_boundary_tests.cpp $(LDLIBS) -o $@

$(PHY_TEST): $(LIB_OBJS) tests/phy_tests.cpp tests/spec_fixture.hpp $(BUILD_CONFIG) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) $(LIB_OBJS) tests/phy_tests.cpp $(LDLIBS) -o $@

$(CONFIG_TEST): $(LIB_OBJS) tests/config_tests.cpp $(BUILD_CONFIG) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) $(LIB_OBJS) tests/config_tests.cpp $(LDLIBS) -o $@

$(MODEL_CONFIG_TEST): $(LIB_OBJS) tests/model_config_tests.cpp $(BUILD_CONFIG) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) $(LIB_OBJS) tests/model_config_tests.cpp $(LDLIBS) -o $@

$(SCHEDULER_CONTRACT_TEST): $(LIB_OBJS) tests/scheduler_contract_tests.cpp $(BUILD_CONFIG) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) $(LIB_OBJS) tests/scheduler_contract_tests.cpp $(LDLIBS) -o $@

$(REFACTOR_TEST): $(LIB_OBJS) tests/refactor_tests.cpp include/hbm_sim/config/fields.hpp include/hbm_sim/config/parse.hpp include/hbm_sim/controller/request_queues.hpp $(BUILD_CONFIG) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) $(LIB_OBJS) tests/refactor_tests.cpp $(LDLIBS) -o $@

.PHONY: refactor-test
refactor-test: $(REFACTOR_TEST)
	./$(REFACTOR_TEST)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_CONFIG): | $(BUILD_DIR)
	@rm -f $(BUILD_DIR)/.build-config $(BUILD_DIR)/.build-config-*
	@printf '%s\n' \
		'CXX=$(CXX)' \
		'CXX_ID=$(CXX_ID)' \
		'CPPFLAGS=$(CPPFLAGS)' \
		'CXXFLAGS=$(CXXFLAGS)' \
		'LDFLAGS=$(LDFLAGS)' \
		'LDLIBS=$(LDLIBS)' > $@

run: $(TARGET)
	./$(TARGET) --config configs/hbm.cfg --standard hbm4 \
		--pattern stream --requests 10000

# examples/test 都显式用 bash 运行，避免依赖脚本文件执行位。
examples: $(TARGET)
	bash ./examples/run_hbm4_stream.sh
	bash ./examples/run_hbm3_random.sh
	bash ./examples/run_lpddr6_trace.sh
	bash ./examples/run_lpddr5_trace.sh

examples_hbm4: $(TARGET)
	bash ./examples/run_hbm4_stream.sh
examples_hbm3: $(TARGET)
	bash ./examples/run_hbm3_random.sh
examples_lpddr6: $(TARGET)
	bash ./examples/run_lpddr6_trace.sh
examples_lpddr5: $(TARGET)
	bash ./examples/run_lpddr5_trace.sh

smoke: $(TARGET)
	HBM_SIM_BIN=./$(TARGET) HBM_SIM_SOURCE_DIR=. bash ./tests/smoke.sh

sequence-test: $(SEQUENCE_TEST)
	./$(SEQUENCE_TEST)

phy-test: $(PHY_TEST)
	./$(PHY_TEST)

config-test: $(CONFIG_TEST) $(MODEL_CONFIG_TEST)
	./$(CONFIG_TEST) .
	./$(MODEL_CONFIG_TEST)

.PHONY: scheduler-contract-test result-test
scheduler-contract-test: $(SCHEDULER_CONTRACT_TEST)
	./$(SCHEDULER_CONTRACT_TEST)

$(RESULT_VALUE_TEST): $(LIB_OBJS) tests/result_value_tests.cpp $(BUILD_CONFIG) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) $(LIB_OBJS) tests/result_value_tests.cpp $(LDLIBS) -o $@

result-test: $(TARGET) $(RESULT_VALUE_TEST)
	./$(RESULT_VALUE_TEST)
	python3 tests/result_tests.py ./$(TARGET)

phy-smoke: $(TARGET)
	HBM_SIM_BIN=./$(TARGET) HBM_SIM_SOURCE_DIR=. bash ./tests/phy_smoke.sh

visualization-smoke: $(TARGET)
	HBM_SIM_BIN=./$(TARGET) HBM_SIM_SOURCE_DIR=. bash ./tests/visualization_smoke.sh

multistack-backend-smoke: $(TARGET)
	HBM_SIM_BIN=./$(TARGET) HBM_SIM_SOURCE_DIR=. bash ./tests/multistack_backend_smoke.sh

multistack-demos-smoke: $(TARGET)
	HBM_SIM_BIN=./$(TARGET) HBM_SIM_SOURCE_DIR=. bash ./tests/multistack_demos_smoke.sh

architecture-sweep-smoke: $(TARGET)
	HBM_SIM_BIN=./$(TARGET) HBM_SIM_SOURCE_DIR=. bash ./tests/architecture_sweep_smoke.sh

architecture-sweep: $(TARGET)
	python3 ./experiments/architecture_sweep/run.py --binary ./$(TARGET)

visualize-example: $(TARGET)
	HBM_SIM_BIN=./$(TARGET) HBM_SIM_SOURCE_DIR=. bash ./tools/visualize_example.sh

timing-boundary-validation: $(TIMING_BOUNDARY_TEST)
	python3 ./tools/timing_boundary_validation.py --probe ./$(TIMING_BOUNDARY_TEST)

model-validation: $(TARGET)
	python3 ./tools/model_validation.py --binary ./$(TARGET)

performance-validation: $(TARGET)
	python3 ./tools/performance_curve.py --binary ./$(TARGET)

sensitivity-validation: $(TARGET)
	python3 ./tools/sensitivity_uncertainty.py --binary ./$(TARGET)

reference-validation: $(TARGET)
	python3 ./tools/ramulator2_differential.py --binary ./$(TARGET) \
		--ramulator-root "$(RAMULATOR2_ROOT)"

# Compatibility alias. The primary name emphasizes that Ramulator is only an
# external reference for the shared command surface.
ramulator-validation: reference-validation

# Make 兼容验收覆盖核心单元、CLI、配置、可视化、多 Stack/后端和架构实验。
# 性能/敏感性 sweep 与外部 Ramulator 参考仍是显式目标，避免默认验收意外拉长。
test: smoke sequence-test phy-test config-test scheduler-contract-test result-test refactor-test phy-smoke visualization-smoke \
	multistack-backend-smoke multistack-demos-smoke architecture-sweep-smoke \
	timing-boundary-validation model-validation

clean:
	rm -rf $(BUILD_DIR)

# 列出/删除项目根目录下所有 Make/CMake 构建目录。clean-build 与
# clean-outputs 严格分离，不会触碰 outputs/ 或源码目录。
list-builds:
	@find . -maxdepth 1 -type d \( -name build -o -name 'build-*' \) -printf '%f\n' | sort

clean-build:
	@echo "Deleting build directories under $(CURDIR)..."
	@find . -maxdepth 1 -type d \( -name build -o -name 'build-*' \) -print -exec rm -rf -- {} +

# Sanitizer 使用独立目录，并在编译、主程序链接和测试链接阶段使用同一组选项。
sanitize:
	$(MAKE) BUILD_DIR=$(SANITIZER_BUILD_DIR) CXX=$(SANITIZER_CXX) \
		CXXFLAGS='$(SANITIZER_FLAGS)' LDFLAGS='$(SANITIZER_LDFLAGS)' all

sanitize-test:
	$(MAKE) BUILD_DIR=$(SANITIZER_BUILD_DIR) CXX=$(SANITIZER_CXX) \
		CXXFLAGS='$(SANITIZER_FLAGS)' LDFLAGS='$(SANITIZER_LDFLAGS)' test

# 仿真产物统一放在 outputs/。该目标保留 outputs/README.md，只删除其余产物；
# 下一次 CLI 运行会自动重建所需子目录。
clean-outputs:
	@echo "Deleting simulation outputs under outputs/..."
	@mkdir -p outputs
	@find outputs -depth -mindepth 1 ! -path 'outputs/README.md' -print -delete

# 兼容旧命令；delete 与 clean-outputs 语义完全相同。
delete: clean-outputs

-include $(DEPS)
