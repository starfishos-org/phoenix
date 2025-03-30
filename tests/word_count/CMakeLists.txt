set(TEST_NAME word_count)
if(${OS} STREQUAL "CHCORE")
    set(TEST_NAME ${TEST_NAME}.bin)
endif()
add_executable(${TEST_NAME} word_count.c sort.c)

target_link_libraries(${TEST_NAME} PRIVATE ${LINK_LIBS})
