set(VX ${PROJECT_SOURCE_DIR}/vortex-gpu/vortex)
set(VB ${PROJECT_SOURCE_DIR}/build/vortex)
set(VS ${VX}/sim/simx)
set(VCOMMON util softfloat_ext rvfloats)
set(VSIM mem)
set(VCORE processor cluster socket core scheduler cta_dispatcher barrier_unit
    decode decompressor scoreboard sequencer opc_unit dispatcher alu_unit fpu_unit
    lsu_unit sfu_unit wctl_unit csr_unit operands kmu/kmu mem/cache mem/cache_cluster
    mem/memory mem/local_mem mem/mem_coalescer mem/local_mem_switch mem/lsu_mem_adapter
    dtm/debug_module dtm/jtag_dtm dtm/remote_bitbang)
set(VSOURCES api.cpp)
foreach(s IN LISTS VCOMMON)
    list(APPEND VSOURCES ${VX}/sw/common/${s}.cpp)
endforeach()
foreach(s IN LISTS VSIM)
    list(APPEND VSOURCES ${VX}/sim/common/${s}.cpp)
endforeach()
foreach(s IN LISTS VCORE)
    list(APPEND VSOURCES ${VS}/${s}.cpp)
endforeach()
add_library(ss_vortex STATIC ${VSOURCES})
target_include_directories(ss_vortex PRIVATE ${VB}/sw ${VB}/hw ${VS}
    ${VS}/dxa ${VS}/tex ${VS}/om ${VS}/raster ${VS}/rtu ${VS}/kmu ${VS}/mem ${VS}/dtm
    ${VX}/sw/common ${VX}/sim/common ${VX}/sw ${VX}/hw ${VX}/third_party
    ${VX}/third_party/softfloat/source/include)
target_include_directories(ss_vortex PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(ss_vortex PRIVATE VX_CFG_XLEN=32 VX_CFG_XLEN_32)
target_compile_options(ss_vortex PRIVATE -O2 -Wall -Wextra)
target_link_libraries(ss_vortex PUBLIC ${VX}/third_party/softfloat/build/Linux-x86_64-GCC/softfloat.a)
