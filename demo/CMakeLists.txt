cmake_minimum_required(VERSION 3.19)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(ENVSDK $ENV{PLAYDATE_SDK_PATH})
if(NOT "${ENVSDK}" STREQUAL "")
    file(TO_CMAKE_PATH "${ENVSDK}" SDK)
endif()
if(NOT EXISTS "${SDK}")
    message(FATAL_ERROR "Set PLAYDATE_SDK_PATH to the Playdate SDK directory")
endif()

set(PLAYDATE_GAME_NAME pdchess_lab)
set(PLAYDATE_GAME_DEVICE pdchess_lab_DEVICE)
project(${PLAYDATE_GAME_NAME} C ASM)

set(PDCHESS_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(PDCHESS_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/src/main.c"
    "${PDCHESS_ROOT}/src/pdchess.c")

if(TOOLCHAIN STREQUAL "armgcc")
    add_executable(${PLAYDATE_GAME_DEVICE} ${PDCHESS_SOURCES})
    target_include_directories(${PLAYDATE_GAME_DEVICE} PRIVATE
        "${PDCHESS_ROOT}/include")
else()
    add_library(${PLAYDATE_GAME_NAME} SHARED ${PDCHESS_SOURCES})
    target_include_directories(${PLAYDATE_GAME_NAME} PRIVATE
        "${PDCHESS_ROOT}/include")
endif()

include("${SDK}/C_API/buildsupport/playdate_game.cmake")
