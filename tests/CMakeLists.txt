# tests/CMakeLists.txt

file(GLOB TEST_DIRS "*/")

foreach(TEST_DIR ${TEST_DIRS})
    get_filename_component(TEST_NAME ${TEST_DIR} NAME)
    if (${TEST_NAME} STREQUAL "CMakeLists.txt")
        continue()
    endif()
    add_subdirectory(${TEST_DIR})
    if ($(OS) STREQUAL "CHCORE")
        target_link_options(${TEST_NAME} PRIVATE -static)
    endif()
    set(TESTS ${TESTS} ${TEST_NAME})
endforeach()
