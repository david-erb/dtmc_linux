cmake_minimum_required(VERSION 3.20)
project(app C)

include(${CMAKE_CURRENT_LIST_DIR}/../../dthelpers.cmake)
dthelpers_resolve(DTMC_LINUX_DIR dtmc_linux "dtmc_linux repo directory")
dthelpers_resolve(DTMC_BASE_DIR dtmc_base "dtmc_base repo directory")
dthelpers_resolve(DTCORE_DIR dtcore "dtcore repo directory")

add_subdirectory(${DTMC_LINUX_DIR}/dtmc_linux_library  ${CMAKE_BINARY_DIR}/dtmc_linux_library)
add_subdirectory(${DTMC_BASE_DIR}/dtmc_base_benchmarks ${CMAKE_BINARY_DIR}/dtmc_base_benchmarks)
add_subdirectory(${DTMC_BASE_DIR}/dtmc_base_library    ${CMAKE_BINARY_DIR}/dtmc_base_library)
add_subdirectory(${DTCORE_DIR}/dtcore_library          ${CMAKE_BINARY_DIR}/dtcore_library)

add_executable(app src/main.c)

target_link_libraries(app
    PRIVATE
        dtmc_base_benchmarks
        dtmc_base_library
        dtmc_linux_library
        dtcore_library
)
