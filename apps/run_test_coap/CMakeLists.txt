cmake_minimum_required(VERSION 3.20)
project(app C)

# Bring in the three libraries (each has its own CMakeLists)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../dtcore_library      ${CMAKE_BINARY_DIR}/dtcore_library)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../dtmc_linux_library ${CMAKE_BINARY_DIR}/dtmc_linux_library)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../dtmc_linux_tests   ${CMAKE_BINARY_DIR}/dtmc_linux_tests)

# main() lives in the app
add_executable(app
    src/main.c
)

# link to tests, which propagates dtmc_linux_library and dtcore_library
target_link_libraries(app
    PRIVATE
        dtmc_linux_tests 
)
