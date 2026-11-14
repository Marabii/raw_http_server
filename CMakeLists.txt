cmake_minimum_required(VERSION 3.15)

project(RawHttpServer C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Default to Debug if no build type is specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to 'Debug' as none was specified.")
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose the type of build (Debug, Release, RelWithDebInfo, MinSizeRel)." FORCE)
endif()

# Option to enable AddressSanitizer (and UndefinedBehaviorSanitizer)
option(ENABLE_ASAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

# Keep the executable at <build-dir>/http_server (not <build-dir>/app/http_server).
# The Dockerfile and .vscode/launch.json rely on this path.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Warnings and sanitizer flags shared by every target.
# Targets opt in with: target_link_libraries(<target> PRIVATE rhs_build_options)
add_library(rhs_build_options INTERFACE)

if(MSVC)
    target_compile_options(rhs_build_options INTERFACE /W4)
else()
    target_compile_options(rhs_build_options INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
endif()

if(ENABLE_ASAN)
    if(MSVC)
        target_compile_options(rhs_build_options INTERFACE /fsanitize=address)
    else()
        # -fno-omit-frame-pointer ensures readable stack traces on crash
        target_compile_options(rhs_build_options INTERFACE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
            -g
        )
        target_link_options(rhs_build_options INTERFACE
            -fsanitize=address,undefined
        )
    endif()
endif()

add_subdirectory(server)   # the engine: static library `rhs`
add_subdirectory(app)      # the URL shortener: executable `http_server`

## Address Sanitizer:
### Configure and build:

### cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
### cmake --build build-asan

### Run:
### ./build-asan/http_server

# ------------------------

## Valgrind:
### Configure and build:

### cmake -B build-valgrind -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=OFF
### cmake --build build-valgrind

### Run:
### valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./build-valgrind/http_server

# ------------------------

## Release mode:
### Configure and build:

### cmake -B build-release -DCMAKE_BUILD_TYPE=Release -DENABLE_ASAN=OFF
### cmake --build build-release

### Run:
### ./build-release/http_server
