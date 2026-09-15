find_package(verilator REQUIRED HINTS /usr/share/verilator)
set(HF ${PROJECT_SOURCE_DIR}/vortex-gpu/vortex/third_party/hardfloat/source)
set(LD_RTL ${CMAKE_CURRENT_SOURCE_DIR}/rtl)
set(LD_SOURCES ${LD_RTL}/logic_die_top.sv ${LD_RTL}/ld_weight_bank.sv
    ${LD_RTL}/ld_radix_topk.sv ${LD_RTL}/ld_fp32.sv ${LD_RTL}/ld_hardfloat_clz.sv
    ${HF}/HardFloat_primitives.v ${HF}/HardFloat_rawFN.v
    ${HF}/isSigNaNRecFN.v ${HF}/fNToRecFN.v ${HF}/recFNToFN.v
    ${HF}/addRecFN.v ${HF}/mulRecFN.v ${HF}/RISCV/HardFloat_specialize.v)
add_library(ss_gate_model STATIC model_anchor.cpp)
verilate(ss_gate_model SOURCES ${LD_SOURCES} TOP_MODULE logic_die_top PREFIX Vlogic_die_top
    TRACE INCLUDE_DIRS ${HF} ${HF}/RISCV VERILATOR_ARGS -Wno-fatal)
add_executable(gate_unit tests/gate_unit.cpp)
target_link_libraries(gate_unit PRIVATE ss_gate_model)
