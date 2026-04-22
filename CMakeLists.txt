cmake_minimum_required(VERSION 3.25)

project(chesserazade
    VERSION 1.2.0
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
option(CHESSERAZADE_USE_PEXT
    "Build the BMI2 (PEXT) slider-attacks variant. Requires a CPU with BMI2."
    OFF)

add_library(chesserazade_core STATIC
    src/board/bitboard.cpp
    src/board/board8x8_mailbox.cpp
    src/board/board_bitboard.cpp
    src/board/magic.cpp
    src/board/zobrist.cpp
    src/eval/evaluator.cpp
    src/game/game.cpp
    src/io/fen.cpp
    src/io/move.cpp
    src/io/pgn.cpp
    src/io/pgn_index.cpp
    src/io/san.cpp
    src/move_generator/bitboard_move_generator.cpp
    src/move_generator/move_generator.cpp
    src/net/curl_fetcher.cpp
    src/search/game_analyzer.cpp
    src/search/search.cpp
    src/search/puzzle_solver.cpp
    src/search/transposition_table.cpp
)
target_include_directories(chesserazade_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
# Bake the repo root into the library so the magic-bitboards
# loader can find data/magics.txt from any cwd even when the
# CHESSERAZADE_MAGICS env var is unset and the binary has
# moved out of its usual build-tree neighbourhood.
target_compile_definitions(chesserazade_core PRIVATE
    CHESSERAZADE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
# PEXT path — opt-in via -DCHESSERAZADE_USE_PEXT=ON. Adds the
# BMI2 TU and passes -mbmi2 to that file only, keeping the
# rest of the library portable to non-BMI2 CPUs.
if(CHESSERAZADE_USE_PEXT)
    target_sources(chesserazade_core PRIVATE src/board/attacks_pext.cpp)
    target_compile_definitions(chesserazade_core PRIVATE CHESSERAZADE_USE_PEXT)
    set_source_files_properties(src/board/attacks_pext.cpp
        PROPERTIES COMPILE_OPTIONS "-mbmi2")
endif()
target_link_libraries(chesserazade_core
    PRIVATE chesserazade_warnings
)

# --------------------------------------------------------------------------
# Command-line executable.
# --------------------------------------------------------------------------
add_executable(chesserazade
    src/main.cpp
    src/cli/command_dispatch.cpp
    src/cli/cmd_analyze.cpp
    src/cli/cmd_fetch.cpp
    src/cli/cmd_magics_gen.cpp
    src/cli/cmd_moves.cpp
    src/cli/cmd_perft.cpp
    src/cli/cmd_repl.cpp
    src/cli/cmd_show.cpp
    src/cli/cmd_solve.cpp
    src/cli/cmd_uci.cpp
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
# Qt6 graphical analyzer — optional, off by default. Builds when Qt6 is
# installed and CHESSERAZADE_BUILD_ANALYZER=ON (auto-detected: on if Qt6
# is available). The analyzer links chesserazade_core directly; it is
# deliberately NOT a UCI client — the search tree it displays needs
# observer hooks UCI does not expose.
# --------------------------------------------------------------------------
option(CHESSERAZADE_BUILD_ANALYZER
    "Build the Qt6 graphical analyzer (chesserazade-analyzer)"
    ON)
if(CHESSERAZADE_BUILD_ANALYZER)
    find_package(Qt6 COMPONENTS Widgets Concurrent QUIET)
    if(Qt6_FOUND)
        qt_standard_project_setup()
        qt_add_executable(chesserazade-analyzer
            analyzer/main.cpp
            analyzer/main_window.cpp
            analyzer/main_window.hpp
        )
        target_include_directories(chesserazade-analyzer
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
        )
        target_link_libraries(chesserazade-analyzer
            PRIVATE
                chesserazade_core
                chesserazade_warnings
                Qt6::Widgets
                Qt6::Concurrent
        )
    else()
        message(STATUS
            "Qt6 not found — skipping chesserazade-analyzer. "
            "Install qt6-base-dev (or equivalent) to enable it.")
    endif()
endif()

# --------------------------------------------------------------------------
# Tests — pulled via FetchContent so the dev only needs a network once.
# --------------------------------------------------------------------------
option(CHESSERAZADE_BUILD_TESTS "Build the test suite" ON)
if(CHESSERAZADE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
