cmake_minimum_required(VERSION 3.16)

project(virtual_ir_remote_engine
    VERSION 1.0.0
    DESCRIPTION "Simulated NEC infrared remote control and virtual TV device"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(VIRE_BUILD_TESTS "Build the unit and integration tests" ON)
option(VIRE_ENABLE_SANITIZERS "Enable AddressSanitizer/LeakSanitizer/UBSan (GCC/Clang, non-Windows)" OFF)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Shared compiler warnings
# ---------------------------------------------------------------------------
add_library(vire_warnings INTERFACE)
if(MSVC)
    target_compile_options(vire_warnings INTERFACE /W4 /permissive- /EHsc)
else()
    target_compile_options(vire_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)
endif()

if(VIRE_ENABLE_SANITIZERS AND NOT WIN32 AND NOT MSVC)
    target_compile_options(vire_warnings INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(vire_warnings INTERFACE -fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Core engine library
# ---------------------------------------------------------------------------
add_library(ir_engine STATIC
    src/IrProtocolEncoder.cpp
    src/UdpSocket.cpp
    src/VirtualTvDevice.cpp
    src/RemoteController.cpp
)

target_include_directories(ir_engine PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(ir_engine PUBLIC cxx_std_17)
target_link_libraries(ir_engine
    PUBLIC  Threads::Threads
    PRIVATE vire_warnings)

if(WIN32)
    target_link_libraries(ir_engine PUBLIC ws2_32)
    target_compile_definitions(ir_engine PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
endif()

# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------
add_executable(virtual_ir_remote src/main.cpp)
target_link_libraries(virtual_ir_remote PRIVATE ir_engine vire_warnings)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(VIRE_BUILD_TESTS)
    enable_testing()
    add_executable(ir_engine_tests tests/EngineTests.cpp)
    target_link_libraries(ir_engine_tests PRIVATE ir_engine vire_warnings)
    add_test(NAME ir_engine_tests COMMAND ir_engine_tests)
endif()
