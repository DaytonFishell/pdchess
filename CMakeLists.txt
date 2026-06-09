cmake_minimum_required(VERSION 3.16)
project(pdchess C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(pdchess STATIC src/pdchess.c)
target_include_directories(pdchess PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

if(MSVC)
    target_compile_options(pdchess PRIVATE /W4)
else()
    target_compile_options(pdchess PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(PDCHESS_TESTS_DEFAULT ON)
else()
    set(PDCHESS_TESTS_DEFAULT OFF)
endif()
option(PDCHESS_BUILD_TESTS "Build host-side pdchess tests"
       ${PDCHESS_TESTS_DEFAULT})
if(PDCHESS_BUILD_TESTS)
    enable_testing()
    add_executable(pdchess_tests tests/test_pdchess.c)
    target_link_libraries(pdchess_tests PRIVATE pdchess)
    add_test(NAME pdchess_tests COMMAND pdchess_tests)
endif()
