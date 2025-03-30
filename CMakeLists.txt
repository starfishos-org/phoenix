cmake_minimum_required(VERSION 3.10)
project(phoenix)

set(OS LINUX)
set(ARCH x86_64)

set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests")
set(INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/data")
set(LIB_NAME "phoenix-lib")

set(CMAKE_BUILD_TYPE Release)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)

if (CHCORE_USER_STATIC_APPS)
    set(LINK_LIBS ${LIB_NAME} rpmalloc-static libc-static)
else()
    set(LINK_LIBS ${LIB_NAME} rpmalloc-shared)
endif()

option(BREAKDOWN "Build breakdown" ON)

if(CMAKE_C_COMPILER MATCHES "musl-gcc")
    set(OS CHCORE)
endif()

message(STATUS "OS: ${OS}")

add_subdirectory(${SRC_DIR})
add_subdirectory(${TESTS_DIR})

target_compile_definitions(${LIB_NAME} PUBLIC "_${OS}_" "_${ARCH}_")

if(BREAKDOWN)
    target_compile_definitions(${LIB_NAME} PUBLIC "TIMING")
endif()
