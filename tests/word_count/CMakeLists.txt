add_executable(word_count word_count.c sort.c)
target_link_libraries(${TEST_NAME} PRIVATE ${LIB_NAME})
