cmake_minimum_required(VERSION 3.16)

project(hbm_sim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(HBM_SIM_CONTROLLER_SOURCES
  src/controller/controller.cpp
  src/controller/executor.cpp
  src/controller/refresh.cpp
  src/controller/rfm.cpp
  src/controller/row_policy.cpp
  src/controller/scheduler.cpp
  src/controller/timing.cpp
)

set(HBM_SIM_CONFIG_SOURCES
  src/config/document.cpp
  src/config/model.cpp
)

set(HBM_SIM_CORE_SOURCES
  src/core/addr_map.cpp
  src/core/data.cpp
  src/core/memory_backend.cpp
  src/core/stack_model.cpp
  src/core/system.cpp
)

set(HBM_SIM_DRAM_SOURCES
  src/dram/interface.cpp
  src/dram/jedec.cpp
  src/dram/mem_phy.cpp
  src/dram/profiles.cpp
  src/dram/semantics.cpp
  src/dram/spec.cpp
  src/dram/standard_traits.cpp
  src/dram/state.cpp
)

set(HBM_SIM_FRONTEND_SOURCES
  src/frontend/traffic.cpp
)

set(HBM_SIM_STATS_SOURCES
  src/stats/stats.cpp
  src/stats/result.cpp
)

set(HBM_SIM_VALIDATION_SOURCES
  src/validation/dfi.cpp
  src/validation/trace.cpp
  src/validation/validator.cpp
)

add_library(hbm_sim_core
  ${HBM_SIM_CONFIG_SOURCES}
  ${HBM_SIM_CONTROLLER_SOURCES}
  ${HBM_SIM_CORE_SOURCES}
  ${HBM_SIM_DRAM_SOURCES}
  ${HBM_SIM_FRONTEND_SOURCES}
  ${HBM_SIM_STATS_SOURCES}
  ${HBM_SIM_VALIDATION_SOURCES}
)

target_include_directories(hbm_sim_core PUBLIC include)
target_compile_options(hbm_sim_core PRIVATE -Wall -Wextra -pedantic)
set_target_properties(hbm_sim_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(storagestacked_memsim SHARED integration/online.cpp)
target_link_libraries(storagestacked_memsim PRIVATE hbm_sim_core)
target_include_directories(storagestacked_memsim PUBLIC integration)

add_executable(hbm_sim
  src/cli/help.cpp
  src/cli/main.cpp
)
target_link_libraries(hbm_sim PRIVATE hbm_sim_core)
target_compile_options(hbm_sim PRIVATE -Wall -Wextra -pedantic)

add_executable(sequence_tests tests/sequence_tests.cpp)
target_link_libraries(sequence_tests PRIVATE hbm_sim_core)
target_compile_options(sequence_tests PRIVATE -Wall -Wextra -pedantic)

add_executable(timing_boundary_tests tests/timing_boundary_tests.cpp)
target_link_libraries(timing_boundary_tests PRIVATE hbm_sim_core)
target_compile_options(timing_boundary_tests PRIVATE -Wall -Wextra -pedantic)

add_executable(phy_tests tests/phy_tests.cpp)
target_link_libraries(phy_tests PRIVATE hbm_sim_core)
target_compile_options(phy_tests PRIVATE -Wall -Wextra -pedantic)

add_executable(config_tests tests/config_tests.cpp)
target_link_libraries(config_tests PRIVATE hbm_sim_core)
target_compile_options(config_tests PRIVATE -Wall -Wextra -pedantic)

add_executable(model_config_tests tests/model_config_tests.cpp)
target_link_libraries(model_config_tests PRIVATE hbm_sim_core)
target_compile_options(model_config_tests PRIVATE -Wall -Wextra -pedantic)
add_executable(scheduler_contract_tests tests/scheduler_contract_tests.cpp)
target_link_libraries(scheduler_contract_tests PRIVATE hbm_sim_core)
target_compile_options(scheduler_contract_tests PRIVATE -Wall -Wextra -pedantic)

enable_testing()
add_executable(refactor_tests tests/refactor_tests.cpp)
target_link_libraries(refactor_tests PRIVATE hbm_sim_core)
target_compile_options(refactor_tests PRIVATE -Wall -Wextra -pedantic)
add_test(NAME refactor_tests COMMAND refactor_tests)
add_executable(result_value_tests tests/result_value_tests.cpp)
target_link_libraries(result_value_tests PRIVATE hbm_sim_core)
target_compile_options(result_value_tests PRIVATE -Wall -Wextra -pedantic)
add_test(NAME result_value_tests COMMAND result_value_tests)
add_test(NAME sequence_tests COMMAND sequence_tests)
add_test(NAME timing_boundary_tests COMMAND timing_boundary_tests)
add_test(NAME phy_tests COMMAND phy_tests)
add_test(NAME config_tests COMMAND config_tests ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME model_config_tests COMMAND model_config_tests)
add_test(NAME scheduler_contract_tests COMMAND scheduler_contract_tests)
add_test(NAME phy_smoke COMMAND ${CMAKE_COMMAND} -E env
  HBM_SIM_BIN=$<TARGET_FILE:hbm_sim>
  HBM_SIM_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
  bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/phy_smoke.sh
)
set_tests_properties(phy_smoke PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME visualization_smoke COMMAND ${CMAKE_COMMAND} -E env
  HBM_SIM_BIN=$<TARGET_FILE:hbm_sim>
  HBM_SIM_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
  bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/visualization_smoke.sh)
set_tests_properties(visualization_smoke PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME multistack_backend_smoke COMMAND ${CMAKE_COMMAND} -E env
  HBM_SIM_BIN=$<TARGET_FILE:hbm_sim>
  HBM_SIM_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
  bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/multistack_backend_smoke.sh)
set_tests_properties(multistack_backend_smoke PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME multistack_demos_smoke COMMAND ${CMAKE_COMMAND} -E env
  HBM_SIM_BIN=$<TARGET_FILE:hbm_sim>
  HBM_SIM_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
  bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/multistack_demos_smoke.sh)
set_tests_properties(multistack_demos_smoke PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME architecture_sweep_smoke COMMAND ${CMAKE_COMMAND} -E env
  HBM_SIM_BIN=$<TARGET_FILE:hbm_sim>
  HBM_SIM_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
  bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/architecture_sweep_smoke.sh)
set_tests_properties(architecture_sweep_smoke PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME smoke COMMAND ${CMAKE_COMMAND} -E env
  HBM_SIM_BIN=$<TARGET_FILE:hbm_sim>
  bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke.sh
)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME result_contract COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/result_tests.py $<TARGET_FILE:hbm_sim>)
  add_test(NAME timing_boundary_validation
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/timing_boundary_validation.py
      --probe $<TARGET_FILE:timing_boundary_tests>
  )
  add_test(NAME model_validation
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/model_validation.py
      --binary $<TARGET_FILE:hbm_sim>
  )
  add_test(NAME performance_curve_validation
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/performance_curve.py
      --binary $<TARGET_FILE:hbm_sim>
      --requests 256
      --intervals 32,8,1
      --read-ratios 100,50
  )
  add_test(NAME sensitivity_uncertainty_validation
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/sensitivity_uncertainty.py
      --binary $<TARGET_FILE:hbm_sim>
      --requests 256
      --samples 6
  )
endif()
