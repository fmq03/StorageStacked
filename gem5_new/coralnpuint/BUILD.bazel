# libcoralnpu-gem5.so 的 bazel 构建规则。
#
# 安装位置: $CORALNPU_HOME/gem5int/BUILD（由 coralnpuint/install.sh 拷入）
#
# 有意做成一个**独立的 package**（新目录 gem5int/），而不是往 //hw_sim:BUILD 里
# 加 target。CoralNPU 上游改 hw_sim/BUILD 的概率远高于新增一个我们自己的目录，
# 独立 package 意味着这份构建规则永远不会和上游冲突 —— 需要打补丁的只剩
# core_mini_axi_wrapper.h 里那两个访问器。
#
# includes = ["."] 是必需的：bazel 默认只给 -iquote execroot，于是
# #include "hettrace/writer.h" 会去找 <repo>/hettrace/writer.h（不存在）。
# 加上 includes 之后 -I gem5int 生效，头就落在 gem5int/hettrace/ 下。
#
# 产物 bazel-bin/gem5int/libcoralnpu-gem5.so 即 gem5 CoralNPU SimObject 的
# library 参数。RVV 变体是另一个 target，两者 ABI 完全相同，gem5 侧不需要知道
# 自己 dlopen 的是哪一个（coralnpu_gem5_build_info() 会说）。

load("//rules:coralnpu_v2.bzl", "coralnpu_v2_binary")

package(default_visibility = ["//visibility:public"])

# 会走外部 DDR 的验证内核。tests/cocotb 里的内核全在 TCM 里跑完，一次 AXI
# master 访问都没有 —— 用它们测 tap 只会得到 0 条记录。emits ddr_touch.elf
coralnpu_v2_binary(
    name = "ddr_touch",
    srcs = ["ddr_touch.cc"],
)

cc_library(
    name = "hettrace",
    hdrs = glob(["hettrace/*.h"]),
    includes = ["."],
)

# 共享部分：tap + ABI 实现。RVV 与非 RVV 只差一个 -DENABLE_RVV，
# 而那个宏在 core_mini_axi_wrapper.h 里选择 VRvvCoreMiniAxi / VCoreMiniAxi，
# 所以必须编两遍，不能只链两遍。
cc_library(
    name = "coralnpu_gem5",
    srcs = ["coralnpu_gem5.cc"],
    hdrs = [
        "coralnpu_gem5.h",
        "coralnpu_trace.h",
    ],
    includes = ["."],
    linkstatic = True,
    # alwayslink 不是可选的：下面 linkshared 的 cc_binary 里没有任何东西引用
    # 这些 extern "C" 函数，链接器会把整个 .o 丢掉，产出一个 15 KB 的空 .so，
    # 而且不报任何错 —— 直到 gem5 侧 dlsym 才炸。//hw_sim:core_mini_axi_simulator
    # 设这个标志正是同一个原因。
    alwayslink = True,
    deps = [
        ":hettrace",
        "//hw_sim:core_mini_axi_wrapper",
        "//tests/verilator_sim:elf",
    ],
)

cc_library(
    name = "coralnpu_gem5_rvv",
    srcs = ["coralnpu_gem5.cc"],
    hdrs = [
        "coralnpu_gem5.h",
        "coralnpu_trace.h",
    ],
    copts = ["-DENABLE_RVV"],
    includes = ["."],
    linkstatic = True,
    # alwayslink 不是可选的：下面 linkshared 的 cc_binary 里没有任何东西引用
    # 这些 extern "C" 函数，链接器会把整个 .o 丢掉，产出一个 15 KB 的空 .so，
    # 而且不报任何错 —— 直到 gem5 侧 dlsym 才炸。//hw_sim:core_mini_axi_simulator
    # 设这个标志正是同一个原因。
    alwayslink = True,
    deps = [
        ":hettrace",
        "//hw_sim:core_mini_axi_wrapper",
        "//tests/verilator_sim:elf",
    ],
)

# linkshared 的 cc_binary 是 CoralNPU 自己导出 .so 的既有做法
# （见 //hw_sim:libcoralnpu_simulator.so），照抄以免踩到不同的 PIC 配置。
#
# 版本脚本与符号绑定选项的理由写在 coralnpu_gem5.map 里：本库静态链进了一整套
# SystemC，而 gem5.opt 里也有一套，不隔离的话进程退出时会段错误。
PLUGIN_LINKOPTS = [
    "-Wl,--version-script=$(location coralnpu_gem5.map)",
    # 本库内部的引用优先绑本库自己的定义，而不是全局作用域里 gem5.opt 的同名符号。
    "-Wl,-Bsymbolic",
    # Clang/Verilator's atomic lock-free queries need libatomic on x86-64 too.
    # A shared-library link otherwise succeeds but standalone dlopen can fail.
    "-latomic",
    "-Wl,-z,defs",
]

cc_binary(
    name = "libcoralnpu-gem5.so",
    additional_linker_inputs = ["coralnpu_gem5.map"],
    linkopts = PLUGIN_LINKOPTS,
    linkshared = True,
    deps = [":coralnpu_gem5"],
)

cc_binary(
    name = "libcoralnpu-gem5-rvv.so",
    additional_linker_inputs = ["coralnpu_gem5.map"],
    linkopts = PLUGIN_LINKOPTS,
    linkshared = True,
    deps = [":coralnpu_gem5_rvv"],
)
