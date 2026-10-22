"""Build one native RTL model using the upstream Chisel/Verilator rules."""
load("//rules:chisel.bzl", "chisel_cc_library")

def npu_variant(itcm, dtcm, l0):
    name = "i%d-d%d-l0%d" % (itcm, dtcm, int(l0))
    base = "StorageNpuL0" + ("On" if l0 else "Off")
    if itcm == 8 and dtcm == 32:
        module = base + "Axi"
    elif itcm == 1024 and dtcm == 1024:
        module = base + "HighmemAxi"
    else:
        module = base + "_ITCM%dKB_DTCM%dKBAxi" % (itcm, dtcm)
    parameters = "V" + module + "_parameters.h"
    chisel_cc_library(
        name = name + "_rtl",
        chisel_lib = "//hdl/chisel/src/coralnpu:coralnpu",
        emit_class = "coralnpu.EmitCore",
        module_name = module,
        extra_outs = [parameters, module + ".zip"],
        gen_flags = [
            "--moduleName=" + base, "--useAxi", "--fetchDataBits=128", "--lsuDataBits=128",
            "--enableRvv=True", "--enableFloat=True", "--enableZfbfmin=True",
            "--enableVectorBf16=True", "--enableVerification=True",
            "--enableFetchL0=" + str(l0),
            "--itcmSizeKBytes=" + str(itcm), "--dtcmSizeKBytes=" + str(dtcm),
        ],
        vopts = [
            "-DTB_SUPPORT", "-DVLEN_128", "-DZVE32F_ON",
            "-Wno-WIDTH", "-Wno-CASEINCOMPLETE", "-Wno-LATCH", "-Wno-SIDEEFFECT",
            "-Wno-MULTIDRIVEN", "-Wno-UNOPTFLAT", "-Wno-BLKANDNBLK", "-Wno-CASEX",
            "-Wno-ASCRANGE", "-Wno-WIDTHEXPAND", "-Wno-WIDTHTRUNC", "-Wno-UNSIGNED",
            "-Wno-WIDTHCONCAT", "-Wno-BLKSEQ",
            "-Ihdl/verilog/rvv/design/FPnew/common_cells/inc",
        ],
    )
    native.cc_library(
        name = name + "_native",
        srcs = ["coralnpu_native.cc"],
        hdrs = ["coralnpu_native.h", parameters],
        includes = ["."],
        defines = ["ENABLE_RVV", "CORALNPU_MODEL=V" + module,
                   "CORALNPU_MODEL_FILE=V" + module + ".h",
                   "CORALNPU_PARAMETERS_FILE=native/" + parameters],
        deps = [":%s_rtl_cc" % name, "//hw_sim:configurable_axi_wrapper", "//tests/verilator_sim:elf"],
        alwayslink = True,
    )
    native.cc_binary(
        name = name + ".so", deps = [":" + name + "_native"], linkshared = True,
        additional_linker_inputs = ["coralnpu_native.map"],
        linkopts = ["-Wl,--version-script=$(location coralnpu_native.map)",
                    "-Wl,-Bsymbolic", "-latomic", "-Wl,-z,defs"],
    )
