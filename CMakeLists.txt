cmake_minimum_required(VERSION 3.20)
project(EndfieldAntiKick LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

# MinHook dependency
set(MINHOOK_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/minhook_lib/include")
set(MINHOOK_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/minhook_lib/lib")

# ============================================================================
# anti_afk.dll - Main anti-AFK payload DLL
# ============================================================================
add_library(anti_afk SHARED src/anti_afk.cpp)
target_include_directories(anti_afk PRIVATE ${MINHOOK_INCLUDE_DIR})
target_link_libraries(anti_afk PRIVATE "${MINHOOK_LIB_DIR}/libMinHook.x64.lib")
set_target_properties(anti_afk PROPERTIES
    OUTPUT_NAME "anti_afk"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
)

# ============================================================================
# d3dcompiler_47.dll - Proxy DLL (loader)
# ============================================================================
add_library(proxy_d3dcompiler SHARED src/proxy_d3dcompiler.cpp)
set_target_properties(proxy_d3dcompiler PROPERTIES
    OUTPUT_NAME "d3dcompiler_47"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
)
