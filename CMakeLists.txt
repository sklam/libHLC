#
# Copyright (c) 2016 , Continuum Analytics, Inc.
# All rights reserved.
#

# Require cmake 2.8.12+
cmake_minimum_required (VERSION 2.8.12)

# Refuse to build on anything but linux
if(APPLE OR WIN32)
    message(FATAL_ERROR "libHLC can only be built on linux.")
endif(APPLE OR WIN32)

# project name
project (libHLC)

# The version number
set (libHLC_VERSION_MAJOR 0)
set (libHLC_VERSION_MINOR 1)
set (libHLC_VERSION ${libHLC_VERSION_MAJOR}.${libHLC_VERSION_MINOR})
set (libHLC_SOVERSION 0.1.0) # the .soversion of the shared lib.

enable_language(CXX)


# CMAKE 3.1.0+ has CXX_STANDARD etc, not present by default on older linux
# just check the flag is supported (most likely gcc).
include(CheckCXXCompilerFlag)
CHECK_CXX_COMPILER_FLAG(-std=c++11 CXX_HAS_CXX11)
if(CXX_HAS_CXX11)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -fno-rtti -Wall -std=c++11")
else()
    message(FATAL_ERROR "Compiler must support C++11")
endif()

# Find llvm cmake tooling.
# Set -DLLVM_DIR=/PATH/TO/LLVM_BUILD_DIRECTORY/lib/cmake/llvm/
# See http://llvm.org/docs/CMake.html#embedding-llvm-in-your-project
# TODO: when LLVM 3.9 is released, document that this var can be set to
# the actual installation location of `LLVMConfig.cmake`.
find_package(LLVM REQUIRED CONFIG)

if(LLVM_PACKAGE_VERSION VERSION_LESS 3.9)
    message(FATAL_ERROR "llvm version must be 3.9+")
endif()

message(STATUS "Found LLVM version: ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake found in: ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# google test
#add_subdirectory(external/gtest_1.7.0)
include_directories(external/gtest_1.7.0/include)

# turn on testing
enable_testing()

# hlc code
add_subdirectory(libhlc)

message(
"
------------------------------------
|           Build Summary          |
------------------------------------
Building...........: ${CMAKE_PROJECT_NAME} version ${libHLC_VERSION}
LLVM version.......: ${LLVM_PACKAGE_VERSION}
LLVM location......: ${LLVM_DIR}
C++ Compiler.......: ${CMAKE_CXX_COMPILER}
C++ Flags..........: ${CMAKE_CXX_FLAGS}
")
