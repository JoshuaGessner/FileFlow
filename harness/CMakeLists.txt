# harness/ — recorded-frame capture and replay (component C17).
#
# Depends only on core/, never on sim/. A replayed capture must exercise the production decode
# chain and nothing else; if the harness could reach into the simulator, a recorded dataset
# would stop being evidence about live behaviour.
add_library(fileflow_harness STATIC
    src/capture.cpp
)

target_include_directories(fileflow_harness PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fileflow_harness PUBLIC fileflow_core PRIVATE fileflow_warnings)

set_target_properties(fileflow_harness PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# `ffreplay` — decode a recorded bundle and report the same metrics as ffsim, so replayed and
# simulated runs are directly comparable.
add_executable(ffreplay src/main.cpp)
target_link_libraries(ffreplay PRIVATE fileflow_harness fileflow_core fileflow_warnings)
set_target_properties(ffreplay PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
