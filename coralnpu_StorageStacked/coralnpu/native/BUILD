load(":variants.bzl", "npu_variant")
package(default_visibility = ["//visibility:public"])
cc_library(
    name="coralnpu_native", srcs=["coralnpu_native.cc"], hdrs=["coralnpu_native.h", "//hdl/chisel/src/coralnpu:VRvvCoreMiniAxi_parameters.h"],
    includes=["."], linkstatic=True, alwayslink=True,
    defines=["ENABLE_RVV"],
    deps=["//hw_sim:core_mini_axi_wrapper", "//tests/verilator_sim:elf"],
)
cc_binary(
    name="libcoralnpu-native.so", deps=[":coralnpu_native"], linkshared=True,
    additional_linker_inputs=["coralnpu_native.map"],
    linkopts=["-Wl,--version-script=$(location coralnpu_native.map)",
              "-Wl,-Bsymbolic", "-latomic", "-Wl,-z,defs"],
)

exports_files(["coralnpu_native.cc", "coralnpu_native.h", "coralnpu_native.map"])

[npu_variant(itcm, dtcm, l0) for itcm, dtcm in [(8, 32), (512, 512), (1024, 1024)] for l0 in [False, True]]
