# src/CMakeLists.txt

file(GLOB SOURCES "*.c")

add_library(${LIB_NAME} STATIC ${SOURCES})

target_include_directories(${LIB_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${INCLUDE_DIR})
