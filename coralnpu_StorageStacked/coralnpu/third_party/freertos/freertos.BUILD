load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "freertos",
    srcs = [
        "croutine.c",
        "event_groups.c",
        "list.c",
        "portable/GCC/RISC-V/port.c",
        "portable/GCC/RISC-V/portASM.S",
        "portable/MemMang/heap_4.c",
        "queue.c",
        "stream_buffer.c",
        "tasks.c",
        "timers.c",
    ],
    hdrs = glob([
        "include/*.h",
        "portable/GCC/RISC-V/*.h",
    ]),
    includes = [
        "include",
        "portable/GCC/RISC-V",
    ],
    deps = [
        "@coralnpu_hw//third_party/freertos:config",
    ],
)
