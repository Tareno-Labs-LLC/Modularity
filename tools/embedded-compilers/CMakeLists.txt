cmake_minimum_required(VERSION 3.20)
project(ModularityEmbeddedCompilers LANGUAGES C CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/TinyCC.cmake")
if(ANDROID AND CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    enable_language(ASM)
    add_library(modularity_tinycc_runtime STATIC
        "${MODULARITY_TINYCC_SOURCE_DIR}/lib/lib-arm64.c"
        "${MODULARITY_TINYCC_SOURCE_DIR}/lib/builtin.c"
        "${MODULARITY_TINYCC_SOURCE_DIR}/lib/stdatomic.c"
        "${MODULARITY_TINYCC_SOURCE_DIR}/lib/atomic.S"
        "${MODULARITY_TINYCC_SOURCE_DIR}/lib/alloca.S")
    set_target_properties(modularity_tinycc_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON OUTPUT_NAME tcc1)
endif()
add_executable(tinycc-probe tinycc_probe.c)
target_link_libraries(tinycc-probe PRIVATE modularity_tinycc)
target_compile_definitions(tinycc-probe PRIVATE TCC_PROBE_RUNTIME="${MODULARITY_TINYCC_SOURCE_DIR}")
if(ANDROID)
    target_link_options(tinycc-probe PRIVATE "-Wl,-z,max-page-size=16384")
endif()
option(MODULARITY_BUILD_EMBEDDED_PROBE "Build the mixed C/C++ Cling context and reload probe" OFF)
if(MODULARITY_BUILD_EMBEDDED_PROBE)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/EmbeddedCompilers.cmake")
    add_executable(embedded-probe embedded_probe.cpp ../../src/EmbeddedScript.cpp)
    target_compile_features(embedded-probe PRIVATE cxx_std_17)
    target_compile_definitions(embedded-probe PRIVATE MODULARITY_HAS_EMBEDDED_COMPILERS=1)
    target_include_directories(embedded-probe PRIVATE ../../src)
    target_link_libraries(embedded-probe PRIVATE modularity_embedded_compilers dl)
    target_link_options(embedded-probe PRIVATE "-Wl,-z,max-page-size=16384" "-Wl,--export-dynamic")
endif()
