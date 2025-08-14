# tests/CMakeLists.txt

file(GLOB TEST_DIRS "*/")

foreach(TEST_DIR ${TEST_DIRS})
    get_filename_component(TEST_NAME ${TEST_DIR} NAME)
    if (${TEST_NAME} STREQUAL "CMakeLists.txt")
        continue()
    endif()
    add_subdirectory(${TEST_DIR})
    if (${OS} STREQUAL "CHCORE")
        if (CHCORE_USER_STATIC_APPS)
            target_link_options(${TEST_NAME}.bin PRIVATE -static)
        endif()
        set(TESTS ${TESTS} ${TEST_NAME}.bin)
        target_compile_definitions(${TEST_NAME}.bin PRIVATE NO_MMAP)
    else()
        set(TESTS ${TESTS} ${TEST_NAME})
    endif()
endforeach()

if (${OS} STREQUAL "CHCORE")
    add_dependencies(phoenix-copy-data ${TESTS})
endif()