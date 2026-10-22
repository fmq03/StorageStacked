# 要求 CMake 至少为 3.20，以支持本工程使用的构建配置。
cmake_minimum_required(VERSION 3.20)
# 声明 CoralNPU 存储集成工程，并启用 C、C++ 与汇编语言。
project(coralnpu_storage LANGUAGES C CXX ASM)
# 将本目录目标使用的 C++ 标准设为 C++17。
set(CMAKE_CXX_STANDARD 17)
# 取消 NDEBUG 宏，保留仿真中的 assert 检查。
add_compile_options(-UNDEBUG)
# 从 integration 的上一级目录取得当前项目根目录的绝对路径。
get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
# 将传入的 STORAGE_PATH 按项目根目录解析为公共存储工程的绝对路径。
get_filename_component(STORAGE_ROOT "${PROJECT_ROOT}/${STORAGE_PATH}" ABSOLUTE)
# 把链路 lane 数和 GT/s 速率写入编译宏，供共享存储实现使用。
add_compile_definitions(AOU_LINK_LANES=${LINK_LANES} AOU_LINK_RATE_GTPS=${LINK_RATE_GTPS}
  # 继续定义每符号位数与周转时间（ns），完成链路编译参数列表。
  AOU_LINK_BITS_PER_SYMBOL=${LINK_BITS_PER_SYMBOL} AOU_LINK_TAT_NS=${LINK_TAT_NS})
# 强制关闭共享存储工程的命令行工具，仅构建集成需要的库。
set(STORAGE_BUILD_CLI OFF CACHE BOOL "" FORCE)
# 指定本项目缓存中的在线 MEMSIM 动态库，覆盖同名缓存配置。
set(STORAGE_MEMSIM_LIBRARY "${PROJECT_ROOT}/coralnpu/.cache/build/memsim/libstoragestacked_memsim.so" CACHE FILEPATH "" FORCE)
# 将共享存储工程加入构建，其生成文件放在 storage 子构建目录。
add_subdirectory("${STORAGE_ROOT}" storage)
# 用入口、AXI 主端和连接模块三个源文件生成 coralnpu_sim。
add_executable(coralnpu_sim main.cc axi_master.cc storage.cc)
# 为运行器添加当前目录和 CoralNPU 原生设备接口的头文件搜索路径。
target_include_directories(coralnpu_sim PRIVATE . "${PROJECT_ROOT}/coralnpu/native")
# 将共享 AXI 存储目标链接到运行器；PRIVATE 表示依赖不向下游传播。
target_link_libraries(coralnpu_sim PRIVATE StorageStacked::axi
  # 继续链接外部传入的 NPU 原生模型库，完成链接依赖列表。
  "${NPU_NATIVE_LIBRARY}")
