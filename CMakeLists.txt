cmake_minimum_required(VERSION 3.25)

project(chesserazade
    VERSION 0.3.0
    DESCRIPTION "Educational classical chess program"
    LANGUAGES CXX)

# --------------------------------------------------------------------------
# Global language settings: C++23, no compiler extensions, warnings-as-errors.
# The HANDOFF mandates this combination; every target in the tree inherits it.
# --------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

# Interface target that carries warning flags. Every library and executable
# in the tree links against this so the flags stay in one place.
add_library(chesserazade_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(chesserazade_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Woverloaded-virtual
        -Wconversion
        -Wsign-conversion
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2
    )
endif()

# --------------------------------------------------------------------------
# Core library — all engine logic lives here. Both the `chesserazade`
# executable and the test binary link against it, so no engine code is
# trapped inside the executable.
# --------------------------------------------------------------------------
add_library(chesserazade_core STATIC
    src/board/board8x8_mailbox.cpp
    src/game/game.cpp
    src/io/fen.cpp
    src/io/move.cpp
    src/io/san.cpp
    src/move_generator/move_generator.cpp
)
target_include_directories(chesserazade_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(chesserazade_core
    PRIVATE chesserazade_warnings
)

# --------------------------------------------------------------------------
# Command-line executable.
# --------------------------------------------------------------------------
add_executable(chesserazade
    src/main.cpp
    src/cli/command_dispatch.cpp
    src/cli/cmd_moves.cpp
    src/cli/cmd_perft.cpp
    src/cli/cmd_show.cpp
    src/cli/cmd_version.cpp
)
target_include_directories(chesserazade
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(chesserazade
    PRIVATE chesserazade_core chesserazade_warnings
)

# --------------------------------------------------------------------------
# Perft benchmark — a separate executable, not part of the test suite.
# Measures speed, not correctness. Tests fail on wrong node counts;
# the benchmark is expected to be run manually for timing feedback.
# --------------------------------------------------------------------------
option(CHESSERAZADE_BUILD_TOOLS "Build developer tools (perft_bench, ...)" ON)
if(CHESSERAZADE_BUILD_TOOLS)
    add_executable(perft_bench tools/perft_bench.cpp)
    target_include_directories(perft_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(perft_bench
        PRIVATE chesserazade_core chesserazade_warnings
    )
endif()

# --------------------------------------------------------------------------
# Tests — pulled via FetchContent so the dev only needs a network once.
# --------------------------------------------------------------------------
option(CHESSERAZADE_BUILD_TESTS "Build the test suite" ON)
if(CHESSERAZADE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
