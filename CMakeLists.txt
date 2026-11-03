cmake_minimum_required(VERSION 3.15)

project(RawHttpServer C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Default to Debug if no build type is specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to 'Debug' as none was specified.")
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose the type of build (Debug, Release, RelWithDebInfo, MinSizeRel)." FORCE)
endif()

# Option to enable AddressSanitizer (and UndefinedBehaviorSanitizer)
option(ENABLE_ASAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_executable(http_server 
    main.c 
    db_driver.c 
    headers_parser.c
)

# Apply strict warnings
if(MSVC)
    target_compile_options(http_server PRIVATE /W4)
else()
    target_compile_options(http_server PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Configure AddressSanitizer if enabled
if(ENABLE_ASAN)
    if(MSVC)
        target_compile_options(http_server PRIVATE /fsanitize=address)
    else()
        # -fno-omit-frame-pointer ensures readable stack traces on crash
        target_compile_options(http_server PRIVATE 
            -fsanitize=address,undefined 
            -fno-omit-frame-pointer 
            -g
        )
        target_link_options(http_server PRIVATE 
            -fsanitize=address,undefined
        )
    endif()
endif()

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
