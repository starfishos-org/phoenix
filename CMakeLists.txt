cmake_minimum_required(VERSION 3.10)
project(phoenix)

set(OS LINUX)
set(ARCH x86_64)

set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests")
set(INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/data")
set(LIB_NAME "phoenix-lib")

set(CMAKE_BUILD_TYPE RELEASE)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)

option(BREAKDOWN "Build breakdown" ON)

if(CMAKE_C_COMPILER MATCHES "musl-gcc")
    set(OS CHCORE)
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g")

if (${OS} STREQUAL "CHCORE")
    if (CHCORE_USER_STATIC_APPS)
        set(LINK_LIBS ${LIB_NAME} rpmalloc-static libc-static)
    else()
        set(LINK_LIBS ${LIB_NAME} rpmalloc-shared)
    endif()
else()
    set(LINK_LIBS ${LIB_NAME})
endif()

if(${OS} STREQUAL "CHCORE")
    add_custom_target(
        phoenix-copy-data
        COMMAND rsync -a ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/* ${build_ramdisk_dir}/
        COMMAND rsync -a ${DATA_DIR}/matrix_datafiles/*.txt ${build_ramdisk_dir}/
        COMMAND rsync -a ${DATA_DIR}/linear_regression_datafiles/key_file_100MB.txt ${build_ramdisk_dir}/
        COMMAND rsync -a ${DATA_DIR}/word_count_datafiles/word_100MB.txt ${build_ramdisk_dir}/
    )
    add_dependencies(ramdisk phoenix-copy-data)
endif()

add_subdirectory(${SRC_DIR})
add_subdirectory(${TESTS_DIR})

target_compile_definitions(${LIB_NAME} PUBLIC "_${OS}_" "_${ARCH}_")

if(BREAKDOWN)
    target_compile_definitions(${LIB_NAME} PUBLIC "TIMING")
endif()

if(${OS} STREQUAL "CHCORE")
    target_compile_definitions(${LIB_NAME} PUBLIC "DSM_ENABLED")
endif()
