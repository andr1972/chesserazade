# Catch2 is pulled via FetchContent so the project has no external
# package-manager requirement. See the HANDOFF for the dependency policy.
include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

add_executable(chesserazade_tests
    test_types.cpp
    test_move.cpp
    test_board.cpp
    test_fen.cpp
)
target_link_libraries(chesserazade_tests
    PRIVATE
        chesserazade_core
        chesserazade_warnings
        Catch2::Catch2WithMain
)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(chesserazade_tests)
