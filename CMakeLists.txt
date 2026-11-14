cmake_minimum_required(VERSION 3.20)
project(ElementalQuest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ELEMENTAL_TOOLS "Build elemental_check and elemental_bench" OFF)

# The engine (engine/). Its scene_demo and net_bench
# extras are built only if their sources ship with it.
set(ELEMENTAL_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/engine")
if(NOT EXISTS "${ELEMENTAL_ENGINE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "No engine found: expected engine/ next to this file.")
endif()
if(ELEMENTAL_TOOLS AND EXISTS "${ELEMENTAL_ENGINE_DIR}/examples/scene_demo.cpp")
    set(ENGINE_EXTRAS ON CACHE BOOL "Build scene_demo and net_bench" FORCE)
else()
    set(ENGINE_EXTRAS OFF CACHE BOOL "Build scene_demo and net_bench" FORCE)
endif()
add_subdirectory("${ELEMENTAL_ENGINE_DIR}" engine)

add_subdirectory(game/elemental_quest)
