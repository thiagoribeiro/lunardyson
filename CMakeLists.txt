cmake_minimum_required(VERSION 3.20)
project(lunardyson VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# Only the ld_* ABI is exported; Luau and yyjson symbols stay internal.
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

option(LD_SANITIZE "Build with AddressSanitizer + UBSan" OFF)
if(LD_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# ThreadSanitizer. The watchdog sets lua_Callbacks::interrupt from its own thread
# while the VM reads it — a pattern Luau's lua.h explicitly sanctions. TSan can't
# see the VM-side read is tolerant, so that one write is suppressed (tsan-suppressions.txt).
option(LD_TSAN "Build with ThreadSanitizer" OFF)
if(LD_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
endif()

# Luau upstream, pinned by the submodule (no fork).
set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/luau EXCLUDE_FROM_ALL)

include(FetchContent)
FetchContent_Declare(yyjson
    GIT_REPOSITORY https://github.com/ibireme/yyjson.git
    GIT_TAG 0.12.0
    GIT_SHALLOW TRUE)
set(YYJSON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(yyjson)

add_library(lunardyson SHARED
    src/runtime.cpp
    src/alloc.cpp
    src/json.cpp
    src/tools.cpp
    src/exec.cpp
    src/check.cpp
    src/watchdog.cpp)
target_include_directories(lunardyson PUBLIC include PRIVATE src)
find_package(Threads REQUIRED)
target_link_libraries(lunardyson PRIVATE Luau.VM Luau.Compiler Luau.Analysis yyjson Threads::Threads)
# Treat Luau's headers as system headers so our -Wall/-Wextra only covers our code.
foreach(dep Luau.VM Luau.Compiler Luau.Analysis Luau.Ast Luau.Common Luau.Config)
    get_target_property(dep_includes ${dep} INTERFACE_INCLUDE_DIRECTORIES)
    if(dep_includes)
        target_include_directories(lunardyson SYSTEM PRIVATE ${dep_includes})
    endif()
endforeach()
target_compile_definitions(lunardyson PRIVATE LD_VERSION="${PROJECT_VERSION}")
if(NOT MSVC)
    target_compile_options(lunardyson PRIVATE -Wall -Wextra)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Export exactly the ld_* ABI (yyjson marks its own API visible).
    target_link_options(lunardyson PRIVATE -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/exports.map)
    set_target_properties(lunardyson PROPERTIES LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/exports.map)
endif()

option(LD_BUILD_TESTS "Build the C smoke test" ON)
if(LD_BUILD_TESTS)
    add_executable(ld_smoke tests/c/smoke.c)
    target_link_libraries(ld_smoke PRIVATE lunardyson)
endif()
