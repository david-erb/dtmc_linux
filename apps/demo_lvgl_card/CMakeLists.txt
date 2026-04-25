cmake_minimum_required(VERSION 3.20)
project(app C)

include(${CMAKE_CURRENT_LIST_DIR}/../../dthelpers.cmake)
dthelpers_resolve(DTMC_LINUX_DIR dtmc_linux "dtmc_linux repo directory")
dthelpers_resolve(DTMC_BASE_DIR dtmc_base "dtmc_base repo directory")
dthelpers_resolve(DTCORE_DIR dtcore "dtcore repo directory")

find_package(SDL2 REQUIRED)

set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)

add_subdirectory(/opt/lvgl ${CMAKE_BINARY_DIR}/lvgl)

add_library(dtmc_lvgl INTERFACE)
target_link_libraries(dtmc_lvgl INTERFACE lvgl SDL2::SDL2)
target_include_directories(dtmc_lvgl INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(dtmc_lvgl
    INTERFACE
        LV_CONF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h"
)

add_subdirectory(${DTMC_LINUX_DIR}/dtmc_linux_library  ${CMAKE_BINARY_DIR}/dtmc_linux_library)
add_subdirectory(${DTMC_BASE_DIR}/dtmc_base_library    ${CMAKE_BINARY_DIR}/dtmc_base_library)
add_subdirectory(${DTCORE_DIR}/dtcore_library          ${CMAKE_BINARY_DIR}/dtcore_library)
add_subdirectory(${DTMC_BASE_DIR}/dtmc_base_demos      ${CMAKE_BINARY_DIR}/dtmc_base_demos)

add_executable(app src/main.c)

target_link_libraries(app
    PRIVATE
        dtmc_lvgl
        dtmc_base_demos
        dtmc_linux_library
        
)