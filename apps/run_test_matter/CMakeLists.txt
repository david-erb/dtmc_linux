cmake_minimum_required(VERSION 3.20)
project(app C CXX)

set(CMAKE_CXX_STANDARD 17)

# this is a CMake directory which has definitions of where the CHIP includes and libraries live
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/chip_provider ${CMAKE_BINARY_DIR}/chip_provider)

# Bring in the three libraries (each has its own CMakeLists)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../dtcore_library      ${CMAKE_BINARY_DIR}/dtcore_library)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../dtmc_linux_library ${CMAKE_BINARY_DIR}/dtmc_linux_library)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../dtmc_linux_tests   ${CMAKE_BINARY_DIR}/dtmc_linux_tests)

# main() lives in the app
add_executable(app
    src/main.cpp
)

# Publish this lib's public headers
target_include_directories(app
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(app PRIVATE
  CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE=0
)

# link to tests, which propagates dtmc_linux_library and dtcore_library
# also link with CHIP so we get its static libraries
target_link_libraries(app
    PRIVATE
        dtmc_linux_tests 
        chip::config         # headers + zzz_generated + defines
        chip::core           # libCHIP.a
        chip::appserver      # libCHIPAppServer.a
)
