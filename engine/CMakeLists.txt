cmake_minimum_required(VERSION 3.15)
project(rtl_engine VERSION 0.3.5 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use -O1 to avoid GCC 16 internal compiler errors
set(CMAKE_CXX_FLAGS_RELEASE "-O1 -DNDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O1 -DNDEBUG")

# sim_engine.cpp triggers GCC 16 ICE even at -O1; use -O0
set_source_files_properties(src/sim_engine.cpp PROPERTIES COMPILE_FLAGS "-O0")

# Build our engine as a static library
add_library(rtl_engine STATIC
    # Industrial-grade RTLIL
    src/rtlil.cpp

    # Parser
    src/verilog_parser.cpp
    src/parser_full.cpp
    src/preprocessor.cpp

    # Lint check
    src/lint_check.cpp

    # Synthesis engine (real implementation)
    src/synth_engine.cpp
    src/synth_passes.cpp

    # Optimization passes
    src/optimization_pass.cpp

    # Technology mapping
    src/techmap.cpp

    # Formal verification (SAT-based)
    src/formal_full.cpp

    # Simulator
    src/simulator.cpp
    src/sim_engine.cpp

    # Engine API
    src/rtl_engine.cpp

    # Timing
    src/timing_est.cpp
    src/timing_graph.cpp
    src/timing_analyzer.cpp

    # Other components
    src/lexer.cpp
    src/lexer_full.cpp
    src/synthesis.cpp
    src/simulator_full.cpp
    src/timing_full.cpp
    # src/engine_full.cpp  — deprecated, functionality merged into rtl_engine.cpp
    src/power_analyzer.cpp

    # Liberty parser (NLDM .lib support)
    src/liberty_parser.cpp
)

target_include_directories(rtl_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Position-independent code for linking with Rust
set_target_properties(rtl_engine PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# Install
install(TARGETS rtl_engine
    ARCHIVE DESTINATION lib
)
install(FILES include/rtl_engine.h
    DESTINATION include
)

# Test executables (excluded for now - they use industrial RTLIL types)
# add_executable(test_industrial
#     src/test.cpp
# )
# target_link_libraries(test_industrial rtl_engine)

# add_executable(test_integration
#     src/test_integration.cpp
# )
# target_link_libraries(test_integration rtl_engine)

# add_executable(test_circuits
#     src/test_circuits.cpp
# )
# target_link_libraries(test_circuits rtl_engine)
